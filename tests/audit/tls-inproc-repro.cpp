// Изоляция заявленного бага C1: TLS-клиент и TLS-сервер в одном процессе.
//
// Режимы:
//   minimal <n>  — QSslServer + тривиальный обработчик, без кода продукта
//   proxy   <n>  — OllamaProxy (код продукта) + фейковый апстрим, клиент в том же процессе
//   child   <n>  — OllamaProxy в этом процессе, клиент — дочерним процессом
//   external <n> — как child, но клиент снаружи (curl) — не реализовано, см. README
//   --client <port> <ca|-> <dns> — клиентская половина (для child)
//
// Сборка (без мока и без CMake, только читает src/):
//   g++ -std=c++17 -g -O1 -fPIC -I src $(pkg-config --cflags Qt6Core Qt6Network) \
//       tests/audit/tls-inproc-repro.cpp src/ollamaproxy.cpp src/tokenmap.cpp \
//       src/tlsmanager.cpp -o /tmp/opencode/audit/repro $(pkg-config --libs Qt6Core Qt6Network)
#include "ollamaproxy.h"
#include "tlsmanager.h"
#include "tokenmap.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QProcess>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

static QString g_caPath;
static QString g_dns = QStringLiteral("proxy.test");

struct Result {
    bool handshake = false;
    int status = 0;
    int errors = 0;
    QString why;
};

static Result clientOnce(quint16 port, bool verify)
{
    Result r;
    QSslSocket sock;
    if (verify) {
        QSslConfiguration cfg = sock.sslConfiguration();
        const QList<QSslCertificate> cas = QSslCertificate::fromPath(g_caPath, QSsl::Pem);
        if (cas.isEmpty()) {
            r.why = QStringLiteral("no CA");
            return r;
        }
        cfg.setCaCertificates({cas.value(0)});
        cfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
        sock.setSslConfiguration(cfg);
    }
    QByteArray raw;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);

    QObject::connect(&sock, &QSslSocket::sslErrors, &sock, [&r](const QList<QSslError> &errs) {
        r.errors += errs.size();
        QStringList t;
        for (const QSslError &e : errs)
            t << e.errorString();
        r.why = t.join(QLatin1Char(';'));
    });
    QObject::connect(&sock, &QSslSocket::encrypted, &sock, [&] {
        r.handshake = true;
        sock.write(QByteArrayLiteral("GET /api/tags HTTP/1.1\r\nHost: t\r\n"
                                     "Authorization: Bearer secret-token-1\r\n"
                                     "Connection: close\r\n\r\n"));
        sock.flush();
    });
    QObject::connect(&sock, &QSslSocket::readyRead, &sock, [&] { raw += sock.readAll(); });
    // OMM_READ_ON_DISCONNECT=1 воспроизводит старую тестовую обвязку: readAll()
    // внутри обработчика disconnected (из QSslSocket в этот момент уже
    // разрушается SSL-сессия).
    const bool readOnDisconnect = qEnvironmentVariableIsSet("OMM_READ_ON_DISCONNECT");
    QObject::connect(&sock, &QSslSocket::disconnected, &sock,
                     [&raw, &loop, readOnDisconnect, &sock] {
                         if (readOnDisconnect)
                             raw += sock.readAll(); // ⚠ небезопасно: SSL-сессия уже рушится
                         loop.quit();
                     });
    QObject::connect(&sock, &QSslSocket::errorOccurred, &sock,
                     [&loop](QAbstractSocket::SocketError) {
                         QTimer::singleShot(0, &loop, &QEventLoop::quit);
                     });
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(5000);

    sock.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, g_dns);
    loop.exec();
    if (!raw.isEmpty()) {
        const int eol = raw.indexOf("\r\n");
        const QList<QByteArray> parts = raw.left(eol).split(' ');
        if (parts.size() >= 2)
            r.status = parts.at(1).toInt();
    } else if (r.why.isEmpty()) {
        r.why = QStringLiteral("no response");
    }
    sock.abort();
    return r;
}

class FakeUpstream : public QTcpServer
{
public:
    FakeUpstream()
    {
        QObject::connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = nextPendingConnection()) {
                QObject::connect(s, &QTcpSocket::readyRead, s, [s] {
                    s->readAll();
                    s->write(QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n"
                                              "Connection: close\r\n\r\n{\"ok\":true}"));
                    s->disconnectFromHost();
                });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
};

static QSslConfiguration serverSsl(const TlsManager::TlsMaterial &mat)
{
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setLocalCertificateChain(QSslCertificate::fromPath(mat.certPath, QSsl::Pem));
    cfg.setLocalCertificate(QSslCertificate::fromPath(mat.certPath, QSsl::Pem).value(0));
    QFile kf(mat.keyPath);
    if (!kf.open(QIODevice::ReadOnly)) {
        std::printf("key open failed\n");
        return cfg;
    }
    QSslKey k(&kf, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (k.isNull()) {
        kf.seek(0);
        k = QSslKey(&kf, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    }
    cfg.setPrivateKey(k);
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    cfg.setProtocol(QSsl::TlsV1_2OrLater);
    return cfg;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    if (argc > 1 && QByteArray(argv[1]) == "--client") {
        g_caPath = QString::fromLocal8Bit(argc > 3 ? argv[3] : "-");
        const bool verify = g_caPath != QStringLiteral("-");
        g_dns = QString::fromLocal8Bit(argc > 4 ? argv[4] : "proxy.test");
        const Result r = clientOnce(quint16(QByteArray(argv[2]).toUShort()), verify);
        std::printf("RESULT handshake=%d status=%d errors=%d why=%s\n", r.handshake ? 1 : 0,
                    r.status, r.errors, qPrintable(r.why));
        return 0;
    }

    const QByteArray mode = argc > 1 ? QByteArray(argv[1]) : QByteArray("proxy");
    const int rounds = argc > 2 ? QByteArray(argv[2]).toInt() : 3;

    QTemporaryDir dir;
    TlsManager::setDirectoryOverride(dir.path());
    TlsManager tls;
    QString err;
    const TlsManager::TlsMaterial mat =
        tls.ensure(ProxySettings::TlsMode::Internal, {QStringLiteral("127.0.0.1")}, g_dns, &err);
    if (!mat.ok) {
        std::printf("TLS SETUP FAILED: %s\n", qPrintable(err));
        return 2;
    }
    g_caPath = mat.caPath;

    FakeUpstream upstream;
    if (!upstream.listen(QHostAddress::LocalHost, 0)) {
        std::printf("upstream listen failed\n");
        return 2;
    }

    if (mode == "proxy") {
        OllamaProxy proxy;
        TokenMap map;
        map.setTokens({{QByteArrayLiteral("secret-token-1"), QStringLiteral("laptop")}});
        proxy.setTokenMap(map);
        ProxyConfig cfg;
        cfg.listenAddresses = {QHostAddress::LocalHost};
        cfg.listenPort = 0;
        cfg.upstreamHost = QStringLiteral("127.0.0.1");
        cfg.upstreamPort = upstream.serverPort();
        cfg.tlsEnabled = true;
        cfg.tlsCertPath = mat.certPath;
        cfg.tlsKeyPath = mat.keyPath;
        QString startErr;
        if (!proxy.start(cfg, &startErr)) {
            std::printf("proxy start failed: %s\n", qPrintable(startErr));
            return 2;
        }
        for (int i = 0; i < rounds; ++i) {
            const Result r = clientOnce(proxy.listenPort(), true);
            std::printf("inproc proxy round %d: handshake=%d status=%d errors=%d why=%s\n", i + 1,
                        r.handshake ? 1 : 0, r.status, r.errors, qPrintable(r.why));
            std::fflush(stdout);
        }
        proxy.stop();
    } else if (mode == "minimal") {
        QSslServer server;
        server.setSslConfiguration(serverSsl(mat));
        server.setHandshakeTimeout(10000);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                QObject::connect(s, &QTcpSocket::readyRead, s, [s] {
                    s->readAll();
                    s->write(QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                                              "Connection: close\r\n\r\n{}"));
                    s->disconnectFromHost();
                });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            std::printf("minimal listen failed\n");
            return 2;
        }
        for (int i = 0; i < rounds; ++i) {
            const Result r = clientOnce(server.serverPort(), true);
            std::printf("inproc minimal round %d: handshake=%d status=%d errors=%d why=%s\n", i + 1,
                        r.handshake ? 1 : 0, r.status, r.errors, qPrintable(r.why));
            std::fflush(stdout);
        }
    } else if (mode == "child") {
        OllamaProxy proxy;
        TokenMap map;
        map.setTokens({{QByteArrayLiteral("secret-token-1"), QStringLiteral("laptop")}});
        proxy.setTokenMap(map);
        ProxyConfig cfg;
        cfg.listenAddresses = {QHostAddress::LocalHost};
        cfg.listenPort = 0;
        cfg.upstreamPort = upstream.serverPort();
        cfg.upstreamHost = QStringLiteral("127.0.0.1");
        cfg.tlsEnabled = true;
        cfg.tlsCertPath = mat.certPath;
        cfg.tlsKeyPath = mat.keyPath;
        QString startErr;
        if (!proxy.start(cfg, &startErr)) {
            std::printf("proxy start failed: %s\n", qPrintable(startErr));
            return 2;
        }
        for (int i = 0; i < rounds; ++i) {
            QProcess p;
            p.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--client"), QString::number(proxy.listenPort()), mat.caPath,
                     g_dns});
            p.waitForFinished(10000);
            std::printf("child round %d: %s", i + 1, p.readAllStandardOutput().constData());
            std::fflush(stdout);
        }
        proxy.stop();
    } else if (mode == "proxy-per-round" || mode == "child-per-round") {
        // Как в selftest: прокси и апстрим живут один запрос и уничтожаются.
        const bool perRoundChild = mode == "child-per-round";
        for (int i = 0; i < rounds; ++i) {
            FakeUpstream up;
            up.listen(QHostAddress::LocalHost, 0);
            {
                OllamaProxy proxy;
                TokenMap map;
                map.setTokens({{QByteArrayLiteral("secret-token-1"), QStringLiteral("laptop")}});
                proxy.setTokenMap(map);
                ProxyConfig cfg;
                cfg.listenAddresses = {QHostAddress::LocalHost};
                cfg.listenPort = 0;
                cfg.upstreamHost = QStringLiteral("127.0.0.1");
                cfg.upstreamPort = up.serverPort();
                cfg.tlsEnabled = true;
                cfg.tlsCertPath = mat.certPath;
                cfg.tlsKeyPath = mat.keyPath;
                QString startErr;
                if (!proxy.start(cfg, &startErr)) {
                    std::printf("round %d start failed: %s\n", i + 1, qPrintable(startErr));
                    return 2;
                }
                if (perRoundChild) {
                    QProcess p;
                    p.start(QCoreApplication::applicationFilePath(),
                            {QStringLiteral("--client"), QString::number(proxy.listenPort()),
                             mat.caPath, g_dns});
                    p.waitForFinished(10000);
                    std::printf("round %d: %s", i + 1, p.readAllStandardOutput().constData());
                } else {
                    const Result r = clientOnce(proxy.listenPort(), true);
                    std::printf("round %d: handshake=%d status=%d errors=%d why=%s\n", i + 1,
                                r.handshake ? 1 : 0, r.status, r.errors, qPrintable(r.why));
                }
                std::fflush(stdout);
                proxy.stop();
            }
            // Дать отработать deleteLater() до следующего accept().
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    } else if (mode == "mix") {
        // Смесь: хороший TLS-запрос, клиент без CA, обрыв сразу после TCP,
        // мусор вместо ClientHello. Прокси переиспользуется.
        OllamaProxy proxy;
        TokenMap map;
        map.setTokens({{QByteArrayLiteral("secret-token-1"), QStringLiteral("laptop")}});
        proxy.setTokenMap(map);
        ProxyConfig cfg;
        cfg.listenAddresses = {QHostAddress::LocalHost};
        cfg.listenPort = 0;
        cfg.upstreamHost = QStringLiteral("127.0.0.1");
        cfg.upstreamPort = upstream.serverPort();
        cfg.tlsEnabled = true;
        cfg.tlsCertPath = mat.certPath;
        cfg.tlsKeyPath = mat.keyPath;
        cfg.maxConnections = 1024;
        QString startErr;
        if (!proxy.start(cfg, &startErr)) {
            std::printf("proxy start failed: %s\n", qPrintable(startErr));
            return 2;
        }
        for (int i = 0; i < rounds; ++i) {
            switch (i % 4) {
            case 0: {
                const Result r = clientOnce(proxy.listenPort(), true);
                std::printf("mix %d good: handshake=%d status=%d\n", i + 1, r.handshake ? 1 : 0,
                            r.status);
                break;
            }
            case 1: {
                // Клиент без нашего CA: с точки зрения сервера handshake может
                // завершиться, но клиент рвёт соединение сразу после cert.
                const Result r = clientOnce(proxy.listenPort(), false);
                std::printf("mix %d noca: handshake=%d status=%d errors=%d\n", i + 1,
                            r.handshake ? 1 : 0, r.status, r.errors);
                break;
            }
            case 2: {
                QTcpSocket raw;
                raw.connectToHost(QHostAddress::LocalHost, proxy.listenPort());
                raw.waitForConnected(1000);
                raw.write("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
                raw.waitForBytesWritten(1000);
                raw.abort(); // не TLS — сервер получит мусор
                std::printf("mix %d garbage\n", i + 1);
                break;
            }
            default: {
                QTcpSocket raw;
                raw.connectToHost(QHostAddress::LocalHost, proxy.listenPort());
                raw.waitForConnected(1000);
                raw.abort(); // обрыв сразу после TCP, до ClientHello
                std::printf("mix %d abort\n", i + 1);
                break;
            }
            }
            std::fflush(stdout);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        proxy.stop();
    } else if (mode == "kill-mid") {
        // Активное TLS-соединение + уничтожение прокси/QSslServer «на лету».
        // Классический сценарий двойного delete: сокет уже перевешен на
        // ProxyConnection, а сервер всё ещё считает его своим.
        for (int i = 0; i < rounds; ++i) {
            FakeUpstream stall; // никогда не отвечает
            stall.listen(QHostAddress::LocalHost, 0);
            QSslSocket sock;
            {
                QSslConfiguration cfg = sock.sslConfiguration();
                cfg.setCaCertificates({QSslCertificate::fromPath(mat.caPath, QSsl::Pem).value(0)});
                cfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
                sock.setSslConfiguration(cfg);

                OllamaProxy proxy;
                TokenMap map;
                map.setTokens({{QByteArrayLiteral("secret-token-1"), QStringLiteral("laptop")}});
                proxy.setTokenMap(map);
                ProxyConfig cfg2;
                cfg2.listenAddresses = {QHostAddress::LocalHost};
                cfg2.listenPort = 0;
                cfg2.upstreamHost = QStringLiteral("127.0.0.1");
                cfg2.upstreamPort = stall.serverPort();
                cfg2.tlsEnabled = true;
                cfg2.tlsCertPath = mat.certPath;
                cfg2.tlsKeyPath = mat.keyPath;
                cfg2.idleTimeoutMs = 60000;
                QString startErr;
                if (!proxy.start(cfg2, &startErr)) {
                    std::printf("round %d start failed: %s\n", i + 1, qPrintable(startErr));
                    return 2;
                }
                QObject::connect(&sock, &QSslSocket::encrypted, &sock, [&sock] {
                    sock.write(QByteArrayLiteral("POST /api/chat HTTP/1.1\r\nHost: t\r\n"
                                                 "Authorization: Bearer secret-token-1\r\n"
                                                 "Content-Length: 5\r\n\r\nhello"));
                    sock.flush();
                });
                QObject::connect(&sock, &QSslSocket::sslErrors, &sock,
                                 [](const QList<QSslError> &errs) {
                                     for (const QSslError &e : errs)
                                         std::printf("  sslError: %s\n", qPrintable(e.errorString()));
                                 });
                sock.connectToHostEncrypted(QStringLiteral("127.0.0.1"), proxy.listenPort(), g_dns);
                // Даём handshake+запросу уйти, апстрим молчит -> Relaying.
                QEventLoop wait;
                QTimer::singleShot(200, &wait, &QEventLoop::quit);
                wait.exec();
                std::printf("round %d: destroying proxy with live TLS conn (enc=%d state=%d err=%s)\n",
                            i + 1, sock.isEncrypted() ? 1 : 0, int(sock.state()),
                            qPrintable(sock.errorString()));
                // proxy уничтожается здесь
            }
            sock.abort();
            std::printf("round %d: survived\n", i + 1);
            std::fflush(stdout);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }
    } else {
        std::printf("usage: %s [proxy|minimal|child|proxy-per-round|child-per-round|mix|kill-mid] "
                    "[rounds]\n",
                    argv[0]);
        return 2;
    }

    std::printf("DONE\n");
    return 0;
}
