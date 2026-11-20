// Кто виноват в «handshake ломается при ~30 незавершённых TCP-соединениях»:
// QSslServer (Qt) или код продукта? Здесь — только QSslServer + тривиальный
// обработчик, никакого OllamaProxy.
//   ./qssl-threshold 0 5 10 20 26 30 40 60
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
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
#include <memory>
#include <vector>

static void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    // Берём готовые файлы из TlsManager, чтобы не дублировать openssl-вызовы.
    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);
    if (args.size() < 2) {
        std::printf("usage: %s <cert> <key> [stalled...]\n", argv[0]);
        return 2;
    }
    const QString certPath = args.at(0);
    const QString keyPath = args.at(1);

    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setLocalCertificateChain(QSslCertificate::fromPath(certPath, QSsl::Pem));
    cfg.setLocalCertificate(QSslCertificate::fromPath(certPath, QSsl::Pem).value(0));
    QFile kf(keyPath);
    if (!kf.open(QIODevice::ReadOnly)) {
        std::printf("key open failed\n");
        return 2;
    }
    QSslKey key(&kf, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (key.isNull()) {
        kf.seek(0);
        key = QSslKey(&kf, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    }
    cfg.setPrivateKey(key);
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    cfg.setProtocol(QSsl::TlsV1_2OrLater);

    QSslServer server;
    server.setSslConfiguration(cfg);
    server.setHandshakeTimeout(10000);
    // Гипотеза: дефолт QTcpServer::maxPendingConnections() == 30 и есть порог.
    const int maxPending = qEnvironmentVariableIntValue("OMM_MAXPENDING");
    if (maxPending > 0)
        server.setMaxPendingConnections(maxPending);
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
        std::printf("listen failed\n");
        return 2;
    }

    for (int i = 2; i < args.size(); ++i) {
        const int nstall = args.at(i).toInt();
        std::vector<std::unique_ptr<QTcpSocket>> raw;
        for (int k = 0; k < nstall; ++k) {
            auto s2 = std::make_unique<QTcpSocket>();
            s2->connectToHost(QHostAddress::LocalHost, server.serverPort());
            s2->waitForConnected(1000);
            raw.push_back(std::move(s2));
        }
        pump(200);

        // Валидный TLS-клиент без проверки сертификата (нам важен handshake).
        QSslSocket sock;
        QSslConfiguration c = QSslConfiguration::defaultConfiguration();
        c.setPeerVerifyMode(QSslSocket::VerifyNone);
        sock.setSslConfiguration(c);
        bool enc = false;
        QEventLoop loop;
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&sock, &QSslSocket::encrypted, &sock, [&] {
            enc = true;
            loop.quit();
        });
        QObject::connect(&sock, &QSslSocket::errorOccurred, &sock,
                         [&loop](QAbstractSocket::SocketError) { loop.quit(); });
        QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
        guard.start(4000);
        sock.connectToHostEncrypted(QStringLiteral("127.0.0.1"), server.serverPort());
        loop.exec();
        std::printf("QSslServer(minimal): stalled=%2d -> handshake=%d err=%s\n", nstall,
                    enc ? 1 : 0, qPrintable(sock.errorString()));
        std::fflush(stdout);
        sock.abort();
        for (auto &s2 : raw)
            s2->abort();
        pump(100);
    }
    return 0;
}
