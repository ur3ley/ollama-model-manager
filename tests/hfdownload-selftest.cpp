// Offline test of HfClient::download against a local HTTP server (T-1).
//
// Replaces the stands of stages 1–2 (python servers outside the tree): byte
// exact download, resume from a `.part`, 416 on a complete `.part`, size
// mismatch, a revision change between attempts (B-10), and the token not
// leaving the Hub on a redirect to another host (S-2). The "Hub" listens on
// 127.0.0.1, the "CDN" on 127.0.0.2.
//   ./hfdownload-selftest
#include "hfclient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

struct Request {
    QByteArray host;  // address:port the request reached
    QByteArray path;
    QByteArray authorization;
    QByteArray range;
};

/**
 * Tiny HTTP/1.1 server: one request per connection, then close. Routes:
 *   /<repo>/resolve/<rev>/<file>   the body of files[<rev>/<file>] (Range aware)
 *   redirects[path] = location     302 to `location`
 */
class HttpServer : public QObject
{
public:
    explicit HttpServer(const QHostAddress &address)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                serve(socket);
        });
        m_listening = m_server.listen(address, 0);
    }

    bool isListening() const { return m_listening; }
    QString base() const
    {
        return QStringLiteral("http://%1:%2")
                .arg(m_server.serverAddress().toString())
                .arg(m_server.serverPort());
    }

    QHash<QByteArray, QByteArray> files;      // "<rev>/<file>" -> body
    QHash<QByteArray, QByteArray> redirects;  // path -> Location
    QList<Request> requests;

private:
    void serve(QTcpSocket *socket)
    {
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
            buffer->append(socket->readAll());
            if (!buffer->contains("\r\n\r\n"))
                return;
            respond(socket, *buffer);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    void respond(QTcpSocket *socket, const QByteArray &head)
    {
        const QList<QByteArray> lines = head.left(head.indexOf("\r\n\r\n")).split('\n');
        Request request;
        request.path = lines.value(0).split(' ').value(1);
        request.host = base().mid(7).toLatin1();
        for (const QByteArray &raw : lines.mid(1)) {
            const QByteArray line = raw.trimmed();
            const int colon = line.indexOf(':');
            const QByteArray name = line.left(colon).trimmed().toLower();
            const QByteArray value = line.mid(colon + 1).trimmed();
            if (name == "authorization")
                request.authorization = value;
            else if (name == "range")
                request.range = value;
        }
        requests.append(request);

        QByteArray status = "200 OK";
        QByteArray body;
        QByteArray extra;
        const auto redirect = redirects.constFind(request.path);
        const int resolve = request.path.indexOf("/resolve/");
        if (redirect != redirects.constEnd()) {
            status = "302 Found";
            extra = "Location: " + *redirect + "\r\n";
        } else if (resolve >= 0 && files.contains(request.path.mid(resolve + 9))) {
            const QByteArray data = files.value(request.path.mid(resolve + 9));
            body = data;
            if (request.range.startsWith("bytes=")) {
                const qint64 from = request.range.mid(6, request.range.indexOf('-') - 6).toLongLong();
                if (from >= data.size()) {
                    status = "416 Range Not Satisfiable";
                    body = "error";
                    extra = "Content-Range: bytes */" + QByteArray::number(data.size()) + "\r\n";
                } else {
                    status = "206 Partial Content";
                    body = data.mid(from);
                }
            }
        } else {
            status = "404 Not Found";
        }
        socket->write("HTTP/1.1 " + status + "\r\nContent-Length: "
                      + QByteArray::number(body.size()) + "\r\nConnection: close\r\n" + extra
                      + "\r\n" + body);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    bool m_listening = false;
};

QByteArray payload(int size, char seed)
{
    QByteArray data(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
        data[i] = char(seed + i % 251);
    return data;
}

} // namespace

class HfDownloadTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void downloadsByteExact();
    void resumesFromPart();
    void completePartAnswered416();
    void sizeMismatchIsRejected();
    void revisionChangeRestartsThePart();
    void tokenStaysOnTheHub();
    void tokenFollowsSameHostRedirect();

private:
    bool download(HfClient &client, const QString &file, qint64 expected,
                  const QString &revision = QString(), QString *error = nullptr);
    QByteArray readFinal(const QString &file) const;

    HttpServer m_hub{QHostAddress(QStringLiteral("127.0.0.1"))};
    QTemporaryDir m_dir;
    QString m_dest;
};

void HfDownloadTest::initTestCase()
{
    QVERIFY(m_hub.isListening());
    QVERIFY(m_dir.isValid());
    HfClient::setHubBaseForTest(m_hub.base());
    m_hub.files.insert("main/f.gguf", payload(200000, 'a'));
    m_hub.files.insert("rev1/r.gguf", payload(50000, 'b'));
    m_hub.files.insert("rev2/r.gguf", payload(50000, 'c'));
}

void HfDownloadTest::init()
{
    m_dest = m_dir.filePath(QString::fromLatin1(QTest::currentTestFunction()));
    QDir().mkpath(m_dest);
    m_hub.requests.clear();
}

bool HfDownloadTest::download(HfClient &client, const QString &file, qint64 expected,
                              const QString &revision, QString *error)
{
    QSignalSpy done(&client, &HfClient::downloadFinished);
    client.download(QStringLiteral("repo"), file, m_dest, expected, revision);
    if (!done.wait(10000))
        return false;
    if (error)
        *error = done.first().at(2).toString();
    return done.first().at(0).toBool();
}

QByteArray HfDownloadTest::readFinal(const QString &file) const
{
    QFile f(QDir(m_dest).filePath(file));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void HfDownloadTest::downloadsByteExact()
{
    HfClient client;
    QVERIFY(download(client, QStringLiteral("f.gguf"), 200000));
    QCOMPARE(readFinal(QStringLiteral("f.gguf")), m_hub.files.value("main/f.gguf"));
    QVERIFY(!QFileInfo::exists(QDir(m_dest).filePath(QStringLiteral("f.gguf.part"))));
}

void HfDownloadTest::resumesFromPart()
{
    const QByteArray data = m_hub.files.value("main/f.gguf");
    QFile part(QDir(m_dest).filePath(QStringLiteral("f.gguf.part")));
    QVERIFY(part.open(QIODevice::WriteOnly));
    part.write(data.left(100000));
    part.close();

    HfClient client;
    QVERIFY(download(client, QStringLiteral("f.gguf"), 200000));
    QCOMPARE(m_hub.requests.last().range, QByteArray("bytes=100000-"));
    QCOMPARE(readFinal(QStringLiteral("f.gguf")), data);
}

void HfDownloadTest::completePartAnswered416()
{
    QFile part(QDir(m_dest).filePath(QStringLiteral("f.gguf.part")));
    QVERIFY(part.open(QIODevice::WriteOnly));
    part.write(m_hub.files.value("main/f.gguf"));
    part.close();

    HfClient client;
    QVERIFY(download(client, QStringLiteral("f.gguf"), 200000));
    QCOMPARE(readFinal(QStringLiteral("f.gguf")), m_hub.files.value("main/f.gguf"));
}

void HfDownloadTest::sizeMismatchIsRejected()
{
    HfClient client;
    QString error;
    QVERIFY(!download(client, QStringLiteral("f.gguf"), 123, QString(), &error));
    QVERIFY(error.contains(QStringLiteral("size mismatch")));
    QVERIFY(!QFileInfo::exists(QDir(m_dest).filePath(QStringLiteral("f.gguf"))));
}

void HfDownloadTest::revisionChangeRestartsThePart()
{
    // Half a file from rev1 is on disk; the repository moved on to rev2.
    QFile part(QDir(m_dest).filePath(QStringLiteral("r.gguf.part")));
    QVERIFY(part.open(QIODevice::WriteOnly));
    part.write(m_hub.files.value("rev1/r.gguf").left(20000));
    part.close();
    QFile rev(QDir(m_dest).filePath(QStringLiteral("r.gguf.part.rev")));
    QVERIFY(rev.open(QIODevice::WriteOnly));
    rev.write("rev1");
    rev.close();

    HfClient client;
    QVERIFY(download(client, QStringLiteral("r.gguf"), 50000, QStringLiteral("rev2")));
    // No splice of two commits: started over, from the pinned revision (B-10).
    QVERIFY(m_hub.requests.last().range.isEmpty());
    QVERIFY(m_hub.requests.last().path.contains("/resolve/rev2/"));
    QCOMPARE(readFinal(QStringLiteral("r.gguf")), m_hub.files.value("rev2/r.gguf"));
    QVERIFY(!QFileInfo::exists(rev.fileName()));
}

void HfDownloadTest::tokenStaysOnTheHub()
{
    HttpServer cdn(QHostAddress(QStringLiteral("127.0.0.2")));
    if (!cdn.isListening())
        QSKIP("127.0.0.2 is not available on this host");
    cdn.files.insert("cdn/f.gguf", m_hub.files.value("main/f.gguf"));
    m_hub.redirects.insert("/repo/resolve/main/f.gguf",
                           (cdn.base() + QStringLiteral("/x/resolve/cdn/f.gguf")).toLatin1());

    HfClient client;
    client.setToken(QStringLiteral("hf_secret"));
    QVERIFY(download(client, QStringLiteral("f.gguf"), 200000));
    m_hub.redirects.clear();
    QCOMPARE(m_hub.requests.last().authorization, QByteArray("Bearer hf_secret"));
    QCOMPARE(cdn.requests.size(), 1);
    QVERIFY(cdn.requests.first().authorization.isEmpty());  // S-2
    QCOMPARE(readFinal(QStringLiteral("f.gguf")), m_hub.files.value("main/f.gguf"));
}

void HfDownloadTest::tokenFollowsSameHostRedirect()
{
    m_hub.files.insert("moved/f.gguf", m_hub.files.value("main/f.gguf"));
    m_hub.redirects.insert("/repo/resolve/main/f.gguf", "/other/resolve/moved/f.gguf");

    HfClient client;
    client.setToken(QStringLiteral("hf_secret"));
    QVERIFY(download(client, QStringLiteral("f.gguf"), 200000));
    m_hub.redirects.clear();
    QCOMPARE(m_hub.requests.size(), 2);
    QCOMPARE(m_hub.requests.at(1).authorization, QByteArray("Bearer hf_secret"));
    QCOMPARE(readFinal(QStringLiteral("f.gguf")), m_hub.files.value("main/f.gguf"));
}

QTEST_GUILESS_MAIN(HfDownloadTest)
#include "hfdownload-selftest.moc"
