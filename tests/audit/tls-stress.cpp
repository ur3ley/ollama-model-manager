// Стресс-сценарии жизненного цикла TLS/plain-соединений встроенного прокси.
// Собирается вручную (см. tests/audit/README.md) с -fsanitize=address,undefined.
//
//   ./tls-stress seq <n>       n последовательных TLS-запросов, прокси один
//   ./tls-stress perround <n>  прокси создаётся/уничтожается на каждый запрос
//   ./tls-stress killmid <n>   слушатель уничтожается при живом TLS-соединении
//   ./tls-stress concurrent <n> n одновременных TLS-клиентов
//   ./tls-stress abortplain <n> n циклов: plain-соединение + рваный обрыв на разных стадиях
//   ./tls-stress aborttls <n>  n циклов: TLS-handshake обрывается на разных стадиях
#include "ollamaproxy.h"
#include "tlsmanager.h"
#include "tokenmap.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <vector>

static QString g_caPath;
static QString g_dns = QStringLiteral("proxy.test");

static int countFds()
{
    QDir dir(QStringLiteral("/proc/self/fd"));
    return dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size();
}

static void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

class FakeUpstream : public QTcpServer
{
public:
    explicit FakeUpstream(bool stall = false) : m_stall(stall)
    {
        QObject::connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = nextPendingConnection()) {
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s] {
                    s->readAll();
                    if (m_stall)
                        return;
                    s->write(QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Length: 11\r\n"
                                              "Connection: close\r\n\r\n{\"ok\":true}"));
                    s->disconnectFromHost();
                });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }

private:
    bool m_stall;
};

static QSslConfiguration clientSsl()
{
    QSslConfiguration c = QSslConfiguration::defaultConfiguration();
    const QList<QSslCertificate> cas = QSslCertificate::fromPath(g_caPath, QSsl::Pem);
    if (!cas.isEmpty())
        c.setCaCertificates({cas.value(0)});
    c.setPeerVerifyMode(QSslSocket::VerifyPeer);
    return c;
}

struct TlsOutcome {
    bool encrypted = false;
    int status = 0;
    QString error;
};

static TlsOutcome tlsRound(quint16 port, int graceMs = 3000)
{
    TlsOutcome out;
    QSslSocket sock;
    sock.setSslConfiguration(clientSsl());
    QByteArray raw;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&sock, &QSslSocket::encrypted, &sock, [&] {
        out.encrypted = true;
        sock.write(QByteArrayLiteral("GET /api/tags HTTP/1.1\r\nHost: t\r\n"
                                     "Authorization: Bearer secret-token-1\r\n"
                                     "Connection: close\r\n\r\n"));
        sock.flush();
    });
    QObject::connect(&sock, &QSslSocket::readyRead, &sock, [&] { raw += sock.readAll(); });
    QObject::connect(&sock, &QSslSocket::disconnected, &sock, [&loop] { loop.quit(); });
    QObject::connect(&sock, &QSslSocket::errorOccurred, &sock,
                     [&](QAbstractSocket::SocketError) { QTimer::singleShot(0, &loop, &QEventLoop::quit); });
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(graceMs);
    sock.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, g_dns);
    loop.exec();
    if (!raw.isEmpty()) {
        const QList<QByteArray> parts = raw.left(raw.indexOf("\r\n")).split(' ');
        if (parts.size() >= 2)
            out.status = parts.at(1).toInt();
    } else {
        out.error = sock.errorString();
    }
    sock.abort();
    return out;
}

static OllamaProxy *startProxy(quint16 upstreamPort, const TlsManager::TlsMaterial &mat,
                               int maxConn = 256, bool tls = true)
{
    auto *proxy = new OllamaProxy;
    TokenMap map;
    map.setTokens({{QByteArrayLiteral("secret-token-1"), QStringLiteral("laptop")}});
    proxy->setTokenMap(map);
    ProxyConfig cfg;
    cfg.listenAddresses = {QHostAddress::LocalHost};
    cfg.listenPort = 0;
    cfg.upstreamHost = QStringLiteral("127.0.0.1");
    cfg.upstreamPort = upstreamPort;
    cfg.tlsEnabled = tls;
    if (tls) {
        cfg.tlsCertPath = mat.certPath;
        cfg.tlsKeyPath = mat.keyPath;
    }
    cfg.maxConnections = maxConn;
    QString err;
    if (!proxy->start(cfg, &err)) {
        std::printf("start failed: %s\n", qPrintable(err));
        delete proxy;
        return nullptr;
    }
    return proxy;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString mode = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("seq");
    const int n = argc > 2 ? QByteArray(argv[2]).toInt() : 10;
    // Для seq: третий аргумент — maxConnections (проверка возврата бюджета).
    const int seqMaxConn = argc > 3 ? QByteArray(argv[3]).toInt() : 256;

    QTemporaryDir dir;
    TlsManager::setDirectoryOverride(dir.path());
    TlsManager tls;
    QString err;
    const TlsManager::TlsMaterial mat =
        tls.ensure(ProxySettings::TlsMode::Internal, {QStringLiteral("127.0.0.1")}, g_dns, &err);
    if (!mat.ok) {
        std::printf("TLS setup failed: %s\n", qPrintable(err));
        return 2;
    }
    g_caPath = mat.caPath;

    FakeUpstream upstream;
    upstream.listen(QHostAddress::LocalHost, 0);

    int bad = 0;
    int connectFail = 0;
    if (mode == QLatin1String("seq")) {
        std::unique_ptr<OllamaProxy> proxy(startProxy(upstream.serverPort(), mat, seqMaxConn));
        if (!proxy)
            return 2;
        for (int i = 0; i < n; ++i) {
            const TlsOutcome r = tlsRound(proxy->listenPort());
            if (!r.encrypted || r.status != 200) {
                ++bad;
                std::printf("seq %d: enc=%d status=%d err=%s\n", i + 1, r.encrypted ? 1 : 0,
                            r.status, qPrintable(r.error));
            }
        }
        pump(200);
        const int budget = proxy->activeConnections();
        std::printf("seq maxConn=%d budget=%d\n", seqMaxConn, budget);
        if (budget != 0)
            ++bad;
        proxy->stop();
    } else if (mode == QLatin1String("perround")) {
        for (int i = 0; i < n; ++i) {
            FakeUpstream up; // новый апстрим на круг, как в selftest
            up.listen(QHostAddress::LocalHost, 0);
            std::unique_ptr<OllamaProxy> proxy(startProxy(up.serverPort(), mat));
            if (!proxy)
                return 2;
            const TlsOutcome r = tlsRound(proxy->listenPort());
            if (!r.encrypted || r.status != 200) {
                ++bad;
                std::printf("perround %d: enc=%d status=%d err=%s\n", i + 1, r.encrypted ? 1 : 0,
                            r.status, qPrintable(r.error));
            }
            proxy->stop();
            proxy.reset();
            pump(20);
        }
    } else if (mode == QLatin1String("killmid")) {
        FakeUpstream stall(true);
        stall.listen(QHostAddress::LocalHost, 0);
        for (int i = 0; i < n; ++i) {
            auto proxy = std::unique_ptr<OllamaProxy>(startProxy(stall.serverPort(), mat));
            if (!proxy)
                return 2;
            const quint16 port = proxy->listenPort();
            QSslSocket sock;
            sock.setSslConfiguration(clientSsl());
            QObject::connect(&sock, &QSslSocket::encrypted, &sock, [&sock] {
                sock.write(QByteArrayLiteral("POST /api/chat HTTP/1.1\r\nHost: t\r\n"
                                             "Authorization: Bearer secret-token-1\r\n"
                                             "Content-Length: 5\r\n\r\nhello"));
                sock.flush();
            });
            sock.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, g_dns);
            QElapsedTimer t;
            t.start();
            while (!sock.isEncrypted() && t.elapsed() < 2000)
                pump(20);
            if (!sock.isEncrypted()) {
                ++bad;
                std::printf("killmid %d: handshake failed: %s\n", i + 1,
                            qPrintable(sock.errorString()));
            }
            pump(100);                 // запрос в полёте, апстрим молчит
            proxy->stop();             // активное TLS-соединение + остановка слушателя
            proxy.reset();             // ... и удаление QSslServer
            sock.abort();
            pump(20);
        }
    } else if (mode == QLatin1String("concurrent")) {
        std::unique_ptr<OllamaProxy> proxy(startProxy(upstream.serverPort(), mat, 1024));
        if (!proxy)
            return 2;
        struct Job {
            QSslSocket sock;
            QByteArray raw;
            bool encrypted = false;
        };
        std::vector<std::unique_ptr<Job>> jobs;
        for (int i = 0; i < n; ++i) {
            auto job = std::make_unique<Job>();
            job->sock.setSslConfiguration(clientSsl());
            QObject::connect(&job->sock, &QSslSocket::encrypted, &job->sock, [j = job.get()] {
                j->encrypted = true;
                j->sock.write(QByteArrayLiteral("GET /api/tags HTTP/1.1\r\nHost: t\r\n"
                                                "Authorization: Bearer secret-token-1\r\n\r\n"));
            });
            QObject::connect(&job->sock, &QSslSocket::readyRead, &job->sock,
                             [j = job.get()] { j->raw += j->sock.readAll(); });
            job->sock.connectToHostEncrypted(QStringLiteral("127.0.0.1"), proxy->listenPort(), g_dns);
            jobs.push_back(std::move(job));
        }
        const int deadline = 8000;
        QElapsedTimer t;
        t.start();
        int done = 0;
        while (t.elapsed() < deadline && done < n) {
            pump(50);
            done = 0;
            for (auto &j : jobs)
                if (j->raw.contains("200 OK"))
                    ++done;
        }
        for (auto &j : jobs) {
            if (!j->raw.contains("200 OK")) {
                ++bad;
                std::printf("concurrent: enc=%d bytes=%d\n", j->encrypted ? 1 : 0,
                            int(j->raw.size()));
            }
            j->sock.abort();
        }
        proxy->stop();
    } else if (mode == QLatin1String("abortplain")) {
        std::unique_ptr<OllamaProxy> proxy(startProxy(upstream.serverPort(), mat, 4096));
        if (!proxy)
            return 2;
        const quint16 port = proxy->listenPort();
        for (int i = 0; i < n; ++i) {
            QTcpSocket s;
            s.connectToHost(QHostAddress::LocalHost, port);
            if (!s.waitForConnected(2000)) {
                ++connectFail; // переполнение backlog — не баг прокси, считаем отдельно
                pump(20);
                continue;
            }
            switch (i % 5) {
            case 0: // обрыв сразу
                break;
            case 1: // половина заголовков
                s.write("GET /api/tags HTTP/1.1\r\nHost: t\r\n");
                s.waitForBytesWritten(200);
                break;
            case 2: // без токена + обрыв
                s.write("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
                s.waitForBytesWritten(200);
                break;
            case 3: // валидный токен, тело не доехало
                s.write("POST /x HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer secret-token-1\r\n"
                        "Content-Length: 100\r\n\r\nabc");
                s.waitForBytesWritten(200);
                break;
            default: // RST вместо FIN
                s.write("GET / HTTP/1.1\r\nHost: t\r\n\r\n");
                s.waitForBytesWritten(200);
                s.abort();
                break;
            }
            s.abort();
            pump(2);
        }
        pump(300);
        proxy->stop();
    } else if (mode == QLatin1String("reject")) {
        // Лимит соединений для TLS теперь считается на accept(): до handshake
        // ответить нечем, поэтому сокет закрывается (H-2). Это и проверяем:
        // клиент НЕ должен успеть расшифроваться.
        std::unique_ptr<OllamaProxy> proxy(startProxy(upstream.serverPort(), mat, 0));
        if (!proxy)
            return 2;
        for (int i = 0; i < n; ++i) {
            const TlsOutcome r = tlsRound(proxy->listenPort());
            if (r.encrypted) {
                ++bad;
                std::printf("reject %d: unexpectedly encrypted (status=%d)\n", i + 1, r.status);
            }
            pump(20);
        }
        proxy->stop();
    } else if (mode == QLatin1String("slots")) {
        // Занятие всех слотов «молчащими» соединениями без токена.
        std::unique_ptr<OllamaProxy> proxy(startProxy(upstream.serverPort(), mat, 2));
        if (!proxy)
            return 2;
        std::vector<std::unique_ptr<QTcpSocket>> idle;
        for (int i = 0; i < 2; ++i) {
            auto s2 = std::make_unique<QTcpSocket>();
            s2->connectToHost(QHostAddress::LocalHost, proxy->listenPort());
            s2->waitForConnected(1000);
            idle.push_back(std::move(s2)); // ни байта не отправлено
        }
        pump(100);
        // Третий клиент не обслуживается: бюджет занят двумя сокетами ещё до
        // handshake, поэтому соединение закрывается (H-2).
        const TlsOutcome r = tlsRound(proxy->listenPort());
        std::printf("slots: third client enc=%d status=%d (ожидаем закрытие до handshake)\n",
                    r.encrypted ? 1 : 0, r.status);
        if (r.encrypted)
            ++bad;
        for (auto &s2 : idle)
            s2->abort();
        proxy->stop();
    } else if (mode == QLatin1String("fds")) {
        // Сколько сокетов в handshake живёт мимо maxConnections (после фикса — 0).
        std::unique_ptr<OllamaProxy> proxy(startProxy(upstream.serverPort(), mat, 1));
        if (!proxy)
            return 2;
        const int before = countFds();
        std::vector<std::unique_ptr<QTcpSocket>> raw;
        for (int i = 0; i < 50; ++i) { // TCP есть, ClientHello нет
            auto s2 = std::make_unique<QTcpSocket>();
            s2->connectToHost(QHostAddress::LocalHost, proxy->listenPort());
            s2->waitForConnected(1000);
            raw.push_back(std::move(s2));
        }
        pump(300);
        const int during = countFds();
        std::printf("fds: before=%d after 50 idle TLS sockets=%d (maxConnections=1)\n", before,
                    during);
        // Валидный клиент при заполненном бюджете тоже не обслуживается —
        // но это отказ по лимиту, а не зависание handshake.
        const TlsOutcome r = tlsRound(proxy->listenPort(), 10000);
        std::printf("fds: valid TLS client during flood enc=%d status=%d err=%s\n",
                    r.encrypted ? 1 : 0, r.status, qPrintable(r.error));
        for (auto &s2 : raw)
            s2->abort();
        pump(200);
        proxy->stop();
        // До фикса 50 «висящих» оставляли +40..+80 fd мимо лимита. Теперь
        // бюджет (maxConnections=1) отбивает их на accept — серверных fd ~1.
        if (during - before > 5)
            ++bad;
    } else if (mode == QLatin1String("slotsplain")) {
        // Plain-режим: молчащее соединение сразу становится ProxyConnection
        // и занимает слот на idleTimeout (по умолчанию 3600 с).
        std::unique_ptr<OllamaProxy> proxy(startProxy(upstream.serverPort(), mat, 2, false));
        if (!proxy)
            return 2;
        std::vector<std::unique_ptr<QTcpSocket>> idle;
        for (int i = 0; i < 2; ++i) {
            auto s2 = std::make_unique<QTcpSocket>();
            s2->connectToHost(QHostAddress::LocalHost, proxy->listenPort());
            s2->waitForConnected(1000);
            idle.push_back(std::move(s2)); // ни байта
        }
        pump(200);
        QTcpSocket third;
        QByteArray raw;
        QEventLoop loop;
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&third, &QTcpSocket::connected, &third, [&third] {
            third.write("GET /api/tags HTTP/1.1\r\nHost: t\r\n"
                        "Authorization: Bearer secret-token-1\r\n\r\n");
            third.flush();
        });
        QObject::connect(&third, &QTcpSocket::readyRead, &third, [&] { raw += third.readAll(); });
        QObject::connect(&third, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
        QObject::connect(&third, &QTcpSocket::errorOccurred, &loop, [&loop] { loop.quit(); });
        QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
        guard.start(3000);
        third.connectToHost(QHostAddress::LocalHost, proxy->listenPort());
        loop.exec();
        std::printf("slotsplain raw: %s\n", raw.left(80).constData());
        const QList<QByteArray> parts = raw.left(raw.indexOf("\r\n")).split(' ');
        const int status = parts.size() >= 2 ? parts.at(1).toInt() : 0;
        std::printf("slotsplain: third client status=%d (ожидаем 503, лимит 2 занят молчанием)\n",
                    status);
        if (status != 503)
            ++bad;
        third.abort();
        for (auto &s2 : idle)
            s2->abort();
        proxy->stop();
    } else if (mode == QLatin1String("fdsweep")) {
        // Сколько «молчащих» TCP-соединений на TLS-порт ломают handshake.
        for (int nstall : {20, 25, 30, 40, 50, 100}) {
            std::unique_ptr<OllamaProxy> proxy(startProxy(upstream.serverPort(), mat, 256));
            if (!proxy)
                return 2;
            std::vector<std::unique_ptr<QTcpSocket>> raw;
            for (int i = 0; i < nstall; ++i) {
                auto s2 = std::make_unique<QTcpSocket>();
                s2->connectToHost(QHostAddress::LocalHost, proxy->listenPort());
                s2->waitForConnected(1000);
                raw.push_back(std::move(s2));
            }
            pump(200);
            const TlsOutcome r = tlsRound(proxy->listenPort(), 4000);
            std::printf("stalled=%2d -> valid client enc=%d status=%d err=%s\n", nstall,
                        r.encrypted ? 1 : 0, r.status, qPrintable(r.error));
            std::fflush(stdout);
            for (auto &s2 : raw)
                s2->abort();
            proxy->stop();
            pump(100);
        }
    } else if (mode == QLatin1String("aborttls")) {
        std::unique_ptr<OllamaProxy> proxy(startProxy(upstream.serverPort(), mat, 4096));
        if (!proxy)
            return 2;
        const quint16 port = proxy->listenPort();
        for (int i = 0; i < n; ++i) {
            if (i % 3 == 0) {
                QTcpSocket s; // мусор вместо ClientHello
                s.connectToHost(QHostAddress::LocalHost, port);
                s.waitForConnected(1000);
                s.write("NOT-A-TLS-CLIENT\r\n\r\n");
                s.waitForBytesWritten(200);
                s.abort();
            } else {
                QSslSocket s;
                s.setSslConfiguration(clientSsl());
                if (i % 3 == 1) {
                    // CA не устанавливаем -> клиент отвергнет сервер и оборвётся
                    QSslConfiguration c = QSslConfiguration::defaultConfiguration();
                    c.setPeerVerifyMode(QSslSocket::VerifyPeer);
                    s.setSslConfiguration(c);
                }
                s.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, g_dns);
                pump(30); // рвём в процессе handshake
                s.abort();
            }
            if (i % 50 == 0)
                pump(10);
        }
        pump(300);
        proxy->stop();
    } else {
        std::printf("unknown mode\n");
        return 2;
    }

    pump(300);
    std::printf("DONE mode=%s n=%d bad=%d connectFail=%d\n", qPrintable(mode), n, bad,
                connectFail);
    return bad == 0 ? 0 : 1;
}
