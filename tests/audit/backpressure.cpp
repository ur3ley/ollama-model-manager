// Демонстрация H-1: валидный токен + апстрим, который не читает.
//
// До фикса: прокси читал клиента и копил байты в буфере QTcpSocket апстрима —
// 400 МиБ за 184 мс (rss_growth ≈ sent).
// После фикса: на 4 МиБ буфера апстрима прокси приостанавливает чтение клиента,
// RSS остаётся ~4 МиБ, клиент упирается в TCP-window. Сработает и счётчик
// тела по фактическим байтам (413), если апстрим читает.
//
//   ./backpressure 400     # писать до 400 МиБ, 30 с или пока клиент не встанет
#include "ollamaproxy.h"
#include "tokenmap.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>

static long rssKb()
{
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    for (const QByteArray &line : f.readAll().split('\n')) {
        if (line.startsWith("VmRSS:")) {
            const QList<QByteArray> parts = line.simplified().split(' ');
            return parts.value(1).toLong();
        }
    }
    return -1;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const qint64 limitMb = argc > 1 ? QByteArray(argv[1]).toLongLong() : 200;

    // Апстрим: принимает соединение и НЕ читает.
    QTcpServer upstream;
    upstream.listen(QHostAddress::LocalHost, 0);
    QObject::connect(&upstream, &QTcpServer::newConnection, &upstream, [&upstream] {
        while (QTcpSocket *s = upstream.nextPendingConnection()) {
            s->setParent(&upstream);   // держим, но не читаем
            // Без этого Qt сам вычитывает ядро в безразмерный user-space буфер,
            // и рост RSS показывает буфер тестового апстрима, а не прокси.
            s->setReadBufferSize(4096);
        }
    });

    OllamaProxy proxy;
    TokenMap map;
    map.setTokens({{QByteArrayLiteral("secret-token-1"), QStringLiteral("laptop")}});
    proxy.setTokenMap(map);
    ProxyConfig cfg;
    cfg.listenAddresses = {QHostAddress::LocalHost};
    cfg.listenPort = 0;
    cfg.upstreamHost = QStringLiteral("127.0.0.1");
    cfg.upstreamPort = upstream.serverPort();
    cfg.tlsEnabled = false;
    cfg.maxBodyBytes = 100LL * 1024 * 1024; // штатный лимит продукта
    QString err;
    if (!proxy.start(cfg, &err)) {
        std::printf("start failed: %s\n", qPrintable(err));
        return 2;
    }

    const long rssBefore = rssKb();
    QTcpSocket client;
    client.connectToHost(QHostAddress::LocalHost, proxy.listenPort());
    if (!client.waitForConnected(2000)) {
        std::printf("client connect failed\n");
        return 2;
    }
    // Только chunked: Content-Length отсутствует, значит проверка 413 не срабатывает.
    client.write("POST /api/chat HTTP/1.1\r\nHost: t\r\n"
                 "Authorization: Bearer secret-token-1\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n");
    client.waitForBytesWritten(2000);

    const QByteArray chunk(64 * 1024, 'a');
    QByteArray frame = QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n";
    qint64 sent = 0;
    const qint64 limitBytes = limitMb * 1024 * 1024;
    QElapsedTimer clock;
    clock.start();
    while (sent < limitBytes && clock.elapsed() < 30000) {
        const qint64 n = client.write(frame);
        if (n <= 0)
            break;
        sent += n;
        client.waitForBytesWritten(2000);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (sent % (32 * 1024 * 1024) == 0) {
            std::printf("sent=%lld MiB rss=%ld MiB\n", sent / (1024 * 1024),
                        (rssKb() - rssBefore) / 1024);
            std::fflush(stdout);
        }
        if ((rssKb() - rssBefore) > 1024 * 1024) // > 1 ГиБ роста
            break;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);

    const QByteArray response = client.readAll();
    std::printf("RESULT sent=%lld MiB rss_growth=%ld MiB elapsed=%lld ms response=[%s]\n",
                sent / (1024 * 1024), (rssKb() - rssBefore) / 1024,
                static_cast<long long>(clock.elapsed()), response.left(32).constData());
    client.abort();
    proxy.stop();
    return 0;
}
