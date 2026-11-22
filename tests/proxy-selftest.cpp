// Самопроверка встроенного auth-прокси.
//
// Никакой внешней сети и Ollama не требуется: фейковый апстрим поднимается
// на QTcpServer (127.0.0.1, порт 0), прокси — тоже на порту 0.
//
// Запуск: ./build/proxy-selftest   (код возврата 0 — все проверки прошли)
#include "ollamaproxy.h"
#include "proxyhead.h"
#include "tlsmanager.h"
#include "tokenmap.h"

#include <QCryptographicHash>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QList>
#include <QPair>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;

void report(const char *name, bool ok, const QString &details = QString())
{
    if (ok) {
        ++g_pass;
        std::printf("PASS %s\n", name);
    } else {
        ++g_fail;
        std::printf("FAIL %s%s%s\n", name, details.isEmpty() ? "" : " — ",
                    qPrintable(details));
    }
    std::fflush(stdout);
}

void skip(const char *name, const QString &why)
{
    ++g_skip;
    std::printf("SKIP %s — %s\n", name, qPrintable(why));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Фейковый апстрим (вместо Ollama)
// ---------------------------------------------------------------------------

class FakeUpstream
{
public:
    enum Mode {
        Normal,      // фиксированный JSON-ответ
        Stream,      // ответ двумя чанками с задержкой 100 мс
        EchoHeaders, // тело ответа = полученный заголовочный блок
        EchoBody,    // тело ответа = полученное тело запроса (включая chunked-рамки)
        Blackhole,   // принимает соединение и НЕ читает (для H-1/M-6)
        Silent       // читает запрос, но никогда не отвечает (для M-6)
    };

    explicit FakeUpstream(Mode mode) : m_mode(mode) {}

    ~FakeUpstream() { m_server.close(); }

    bool start()
    {
        if (!m_server.listen(QHostAddress::LocalHost, 0))
            return false;
        QObject::connect(&m_server, &QTcpServer::newConnection, &m_server, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection())
                serve(socket);
        });
        return true;
    }

    quint16 port() const { return m_server.serverPort(); }
    QByteArray lastHead() const { return m_lastHead; }

private:
    struct Conn {
        QByteArray buf;
        QByteArray head;
        bool headDone = false;
        bool chunked = false;
        qint64 contentLength = -1;
        bool responded = false;
    };

    static void respond(QTcpSocket *socket, const QByteArray &body)
    {
        QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                              "Content-Length: "
                              + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n"
                              + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    void serve(QTcpSocket *socket)
    {
        if (m_mode == Blackhole) {
            // Намеренно не читаем. Маленький read buffer обязателен: иначе Qt
            // сам вычитывает ядро в безразмерный user-space буфер, и
            // backpressure не наступает.
            socket->setReadBufferSize(4096);
            QObject::connect(socket, &QTcpSocket::disconnected, socket,
                             &QObject::deleteLater);
            return;
        }
        if (m_mode == Silent) {
            // Запрос вычитываем (иначе клиент упрётся в свой буфер), ответ не шлём.
            QObject::connect(socket, &QTcpSocket::readyRead, socket,
                             [socket] { socket->readAll(); });
            QObject::connect(socket, &QTcpSocket::disconnected, socket,
                             &QObject::deleteLater);
            return;
        }

        auto conn = std::make_shared<Conn>();
        Conn *state = conn.get();

        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, conn, state] {
            state->buf += socket->readAll();

            if (!state->headDone) {
                const int end = state->buf.indexOf("\r\n\r\n");
                if (end < 0)
                    return;
                state->head = state->buf.left(end + 4);
                state->buf.remove(0, end + 4);
                state->headDone = true;
                m_lastHead = state->head;

                const QByteArray lower = state->head.toLower();
                state->chunked = lower.contains("transfer-encoding: chunked");
                const int cl = lower.indexOf("content-length:");
                if (cl >= 0) {
                    const int eol = lower.indexOf("\r\n", cl);
                    bool ok = false;
                    const qint64 value =
                        lower.mid(cl + 15, eol - cl - 15).trimmed().toLongLong(&ok);
                    if (ok)
                        state->contentLength = value;
                }

                if (m_mode == Normal) {
                    state->responded = true;
                    respond(socket, QByteArrayLiteral("{\"ok\":true}"));
                    return;
                }
                if (m_mode == EchoHeaders) {
                    state->responded = true;
                    respond(socket, state->head);
                    return;
                }
                if (m_mode == Stream) {
                    state->responded = true;
                    const QByteArray first = QByteArrayLiteral("{\"part\":1}\n");
                    const QByteArray second = QByteArrayLiteral("{\"part\":2}\n");
                    const QByteArray body = first + second;
                    QByteArray response =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n"
                        "Content-Length: "
                        + QByteArray::number(body.size())
                        + "\r\nConnection: close\r\n\r\n" + first;
                    socket->write(response);
                    socket->flush();
                    // Второй чанк — через 100 мс: если прокси буферизует,
                    // клиент не увидит первый чанк раньше этого таймера.
                    QTimer::singleShot(100, socket, [socket, second] {
                        socket->write(second);
                        socket->disconnectFromHost();
                    });
                    return;
                }
            }

            if (m_mode == EchoBody && !state->responded) {
                const bool complete =
                    state->chunked
                        ? state->buf.contains("0\r\n\r\n")
                        : (state->contentLength >= 0 && state->buf.size() >= state->contentLength);
                if (complete) {
                    state->responded = true;
                    respond(socket, state->buf);
                }
            }
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    Mode m_mode;
    QTcpServer m_server;
    QByteArray m_lastHead;
};

// ---------------------------------------------------------------------------
// Минимальный HTTP-клиент для тестов
// ---------------------------------------------------------------------------

struct Response {
    int status = 0;
    QByteArray head;
    QByteArray body;
    QByteArray raw;
    bool timedOut = false;
};

int parseStatus(const QByteArray &raw)
{
    const int eol = raw.indexOf("\r\n");
    if (eol <= 0)
        return 0;
    const QList<QByteArray> parts = raw.left(eol).simplified().split(' ');
    return parts.size() >= 2 ? parts.at(1).toInt() : 0;
}

Response sendRequest(const QHostAddress &address, quint16 port, const QByteArray &payload,
                     int timeoutMs = 5000)
{
    Response response;
    QTcpSocket socket;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);

    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&socket, &payload] {
        socket.write(payload);
        socket.flush();
    });
    QObject::connect(&socket, &QTcpSocket::readyRead, &socket,
                     [&socket, &response] { response.raw += socket.readAll(); });
    QObject::connect(&socket, &QTcpSocket::disconnected, &socket, [&socket, &response, &loop] {
        response.raw += socket.readAll();
        loop.quit();
    });
    QObject::connect(&socket, &QTcpSocket::errorOccurred, &socket,
                     [&loop](QAbstractSocket::SocketError) { loop.quit(); });
    QObject::connect(&guard, &QTimer::timeout, &loop, [&loop, &response] {
        response.timedOut = true;
        loop.quit();
    });
    guard.start(timeoutMs);

    socket.connectToHost(address, port);
    loop.exec();

    response.status = parseStatus(response.raw);
    const int end = response.raw.indexOf("\r\n\r\n");
    if (end >= 0) {
        response.head = response.raw.left(end + 4);
        response.body = response.raw.mid(end + 4);
    } else {
        response.head = response.raw;
    }
    return response;
}

QByteArray getRequest(const QByteArray &path, const QList<QByteArray> &headers = {})
{
    QByteArray request = "GET " + path + " HTTP/1.1\r\nHost: proxy.test\r\n";
    for (const QByteArray &header : headers)
        request += header + "\r\n";
    request += "Connection: keep-alive\r\n\r\n";
    return request;
}

QByteArray bearer(const char *token)
{
    return QByteArrayLiteral("Authorization: Bearer ") + token;
}

// ---------------------------------------------------------------------------
// Общая обвязка: апстрим + прокси с одним адресом
// ---------------------------------------------------------------------------

struct Fixture {
    FakeUpstream upstream;
    OllamaProxy proxy;
    bool started = false;
    QString error;

    Fixture(FakeUpstream::Mode mode, const ProxyConfig &cfgBase, const TokenMap &tokens)
        : upstream(mode)
    {
        upstream.start();
        ProxyConfig cfg = cfgBase;
        cfg.listenAddresses = {QHostAddress::LocalHost};
        cfg.listenPort = 0;
        cfg.upstreamPort = upstream.port();
        proxy.setTokenMap(tokens);
        started = proxy.start(cfg, &error);
    }

    Response get(const QByteArray &path, const QList<QByteArray> &headers = {})
    {
        return sendRequest(QHostAddress::LocalHost, proxy.listenPort(),
                           getRequest(path, headers));
    }
};

ProxyConfig baseConfig()
{
    ProxyConfig cfg;
    cfg.listenPort = 0;
    cfg.upstreamHost = QStringLiteral("127.0.0.1");
    cfg.idleTimeoutMs = 5000;
    cfg.connectTimeoutMs = 3000;
    return cfg;
}

TokenMap sampleTokens()
{
    TokenMap map;
    map.setTokens({{QByteArrayLiteral("secret-token-1"), QStringLiteral("laptop")},
                   {QByteArrayLiteral("secret-token-2"), QStringLiteral("phone")}});
    return map;
}

// ---------------------------------------------------------------------------
// Тесты 1–9 из docs/proxy.md
// ---------------------------------------------------------------------------

void testNoToken()
{
    Fixture f(FakeUpstream::Normal, baseConfig(), sampleTokens());
    const Response r = f.get(QByteArrayLiteral("/api/tags"));
    report("1. без токена -> 401", f.started && r.status == 401
                                       && r.body.contains("{\"error\":\"unauthorized\"}"),
           QStringLiteral("started=%1 status=%2").arg(f.started).arg(r.status));
}

void testWrongToken()
{
    Fixture f(FakeUpstream::Normal, baseConfig(), sampleTokens());
    const Response r = f.get(QByteArrayLiteral("/api/tags"), {bearer("nope-wrong")});
    report("2. неверный токен -> 401", f.started && r.status == 401,
           QStringLiteral("status=%1").arg(r.status));
}

void testBearerOk()
{
    Fixture f(FakeUpstream::EchoHeaders, baseConfig(), sampleTokens());
    const Response r = f.get(QByteArrayLiteral("/api/tags"), {bearer("secret-token-1")});
    const QByteArray seen = r.body; // апстрим вернул полученные заголовки
    const bool hostOk = seen.contains("Host: 127.0.0.1:" + QByteArray::number(f.upstream.port()));
    const bool deviceOk = seen.contains("X-Ollama-Device: laptop");
    report("3. Bearer -> 200, Host и X-Ollama-Device у апстрима",
           f.started && r.status == 200 && hostOk && deviceOk,
           QStringLiteral("status=%1 host=%2 device=%3").arg(r.status).arg(hostOk).arg(deviceOk));
}

void testApiKeyOk()
{
    Fixture f(FakeUpstream::EchoHeaders, baseConfig(), sampleTokens());
    const Response r =
        f.get(QByteArrayLiteral("/api/tags"), {QByteArrayLiteral("X-Api-Key: secret-token-2")});
    report("4. X-Api-Key -> 200", f.started && r.status == 200
                                      && r.body.contains("X-Ollama-Device: phone"),
           QStringLiteral("status=%1").arg(r.status));
}

void testStreaming()
{
    Fixture f(FakeUpstream::Stream, baseConfig(), sampleTokens());
    if (!f.started) {
        report("5. стриминг: первый чанк раньше конца", false, f.error);
        return;
    }

    QTcpSocket socket;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QElapsedTimer clock;
    qint64 firstAt = -1;
    qint64 totalAt = -1;
    QByteArray raw;

    QObject::connect(&socket, &QTcpSocket::connected, &socket, [&] {
        clock.start();
        socket.write(getRequest(QByteArrayLiteral("/api/chat"), {bearer("secret-token-1")}));
    });
    QObject::connect(&socket, &QTcpSocket::readyRead, &socket, [&] {
        if (firstAt < 0)
            firstAt = clock.elapsed();
        raw += socket.readAll();
    });
    QObject::connect(&socket, &QTcpSocket::disconnected, &socket, [&] {
        totalAt = clock.elapsed();
        raw += socket.readAll();
        loop.quit();
    });
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(5000);

    socket.connectToHost(QHostAddress::LocalHost, f.proxy.listenPort());
    loop.exec();

    const bool bothParts = raw.contains("{\"part\":1}") && raw.contains("{\"part\":2}");
    // Первый чанк апстрима уходит сразу, второй — через 100 мс. Если прокси
    // копит ответ, firstAt будет ≈100 мс.
    const bool fastFirst = firstAt >= 0 && firstAt < 80;
    const bool delayedSecond = firstAt >= 0 && totalAt >= 0 && (totalAt - firstAt) >= 60;
    report("5. стриминг: первый чанк раньше конца",
           bothParts && fastFirst && delayedSecond,
           QStringLiteral("first=%1ms total=%2ms parts=%3").arg(firstAt).arg(totalAt).arg(bothParts));
}

void testChunkedBody()
{
    Fixture f(FakeUpstream::EchoBody, baseConfig(), sampleTokens());
    const QByteArray chunked = QByteArrayLiteral("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    QByteArray request = "POST /api/echo HTTP/1.1\r\nHost: proxy.test\r\n"
                         + bearer("secret-token-1")
                         + "\r\nTransfer-Encoding: chunked\r\n\r\n" + chunked;
    const Response r = sendRequest(QHostAddress::LocalHost, f.proxy.listenPort(), request);
    report("6. chunked-тело дошло байт-в-байт",
           f.started && r.status == 200 && r.body == chunked,
           QStringLiteral("status=%1 got=%2 bytes").arg(r.status).arg(r.body.size()));
}

void testHeaderLimit()
{
    Fixture f(FakeUpstream::Normal, baseConfig(), sampleTokens());
    QByteArray request = "GET /api/tags HTTP/1.1\r\nHost: proxy.test\r\n";
    request += bearer("secret-token-1") + "\r\n";
    request += "X-Filler: " + QByteArray(40000, 'a') + "\r\n\r\n";
    const Response r = sendRequest(QHostAddress::LocalHost, f.proxy.listenPort(), request);
    report("7. заголовки > 32 КиБ -> 431", f.started && r.status == 431,
           QStringLiteral("status=%1").arg(r.status));
}

void testDeviceNotSpoofable()
{
    Fixture f(FakeUpstream::EchoHeaders, baseConfig(), sampleTokens());
    const Response r = f.get(QByteArrayLiteral("/api/tags"),
                             {bearer("secret-token-1"),
                              QByteArrayLiteral("X-Ollama-Device: attacker")});
    const QByteArray seen = r.body;
    const bool ownDevice = seen.contains("X-Ollama-Device: laptop");
    const bool noSpoof = !seen.contains("attacker");
    report("8. клиентский X-Ollama-Device не подменяет устройство",
           f.started && r.status == 200 && ownDevice && noSpoof,
           QStringLiteral("status=%1 own=%2 spoof=%3")
               .arg(r.status)
               .arg(ownDevice)
               .arg(!noSpoof));
}

void testSpecificAddressWhileLoopbackBusy()
{
    static const char *kName = "9. bind на 127.0.0.2 при занятом 127.0.0.1";
    const QHostAddress alias(QStringLiteral("127.0.0.2"));

    // 127.0.0.2 на Linux — отдельный адрес; если недоступен, тест пропускаем.
    QTcpServer probe;
    if (!probe.listen(alias, 0)) {
        skip(kName, QStringLiteral("127.0.0.2 недоступен"));
        return;
    }
    probe.close();

    // Нужен порт, свободный на 127.0.0.2 и занятый на 127.0.0.1.
    QTcpServer blocker;
    quint16 port = 0;
    for (int attempt = 0; attempt < 20 && port == 0; ++attempt) {
        QTcpServer pick;
        if (!pick.listen(alias, 0))
            break;
        const quint16 candidate = pick.serverPort();
        pick.close();
        if (blocker.listen(QHostAddress::LocalHost, candidate))
            port = candidate;
    }
    if (port == 0) {
        skip(kName, QStringLiteral("не удалось занять 127.0.0.1 рядом с 127.0.0.2"));
        return;
    }

    FakeUpstream upstream(FakeUpstream::Normal);
    upstream.start();
    ProxyConfig cfg = baseConfig();
    cfg.listenAddresses = {QHostAddress(QStringLiteral("127.0.0.2"))};
    cfg.listenPort = port;
    cfg.upstreamPort = upstream.port();

    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    const bool started = proxy.start(cfg, &error);

    bool reachable = false;
    if (started) {
        const Response r = sendRequest(QHostAddress(QStringLiteral("127.0.0.2")),
                                       proxy.listenPort(),
                                       getRequest(QByteArrayLiteral("/api/tags"),
                                                  {bearer("secret-token-1")}));
        reachable = r.status == 200;
    }
    report("9. bind на 127.0.0.2 при занятом 127.0.0.1",
           started && reachable && proxy.boundEndpoints().contains(
                                       QStringLiteral("127.0.0.2:%1").arg(port)),
           QStringLiteral("started=%1 reachable=%2 error=%3")
               .arg(started)
               .arg(reachable)
               .arg(error));
    proxy.stop();
    blocker.close();
}

// Дополнительно к списку из ТЗ: лимит тела и лимит соединений.
void testBodyLimit()
{
    Fixture f(FakeUpstream::Normal, baseConfig(), sampleTokens());
    QByteArray request = "POST /api/echo HTTP/1.1\r\nHost: proxy.test\r\n"
                         + bearer("secret-token-1")
                         + "\r\nContent-Length: 200000000\r\n\r\n";
    const Response r = sendRequest(QHostAddress::LocalHost, f.proxy.listenPort(), request);
    report("10. Content-Length > 100 МиБ -> 413", f.started && r.status == 413,
           QStringLiteral("status=%1").arg(r.status));
}

void testConnectionLimit()
{
    ProxyConfig cfg = baseConfig();
    cfg.maxConnections = 0; // любое соединение сверх лимита
    Fixture f(FakeUpstream::Normal, cfg, sampleTokens());
    const Response r = f.get(QByteArrayLiteral("/api/tags"), {bearer("secret-token-1")});
    report("11. лимит соединений -> 503", f.started && r.status == 503,
           QStringLiteral("status=%1").arg(r.status));
}

int countOpenFds()
{
#ifdef Q_OS_LINUX
    const QDir dir(QStringLiteral("/proc/self/fd"));
    return dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).size();
#else
    return -1;
#endif
}

// Жизненный цикл: серия запросов не должна оставлять незакрытых сокетов.
void testNoSocketLeak()
{
    const int before = countOpenFds();
    if (before < 0) {
        skip("12. нет утечки сокетов после 50 запросов", QStringLiteral("/proc/self/fd недоступен"));
        return;
    }

    Fixture f(FakeUpstream::Normal, baseConfig(), sampleTokens());
    int okCount = 0;
    for (int i = 0; i < 50; ++i) {
        const Response r = f.get(QByteArrayLiteral("/api/tags"), {bearer("secret-token-1")});
        if (r.status == 200)
            ++okCount;
    }
    // Даём deleteLater() отработать.
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    const int after = countOpenFds();
    report("12. нет утечки сокетов после 50 запросов",
           f.started && okCount == 50 && after <= before + 2,
           QStringLiteral("ok=%1 fds %2 -> %3").arg(okCount).arg(before).arg(after));
}

} // namespace


// ---------------------------------------------------------------------------
// TLS: свой CA + лист на 127.0.0.1 (docs/proxy.md, «TLS между OPNsense и прокси»)
//
// Клиент — in-process QSslSocket. Раньше он запускался дочерним процессом
// из-за «порчи heap»: настоящая причина — QTcpServer::maxPendingConnections()
// == 30, а QSslServer держал в этой очереди ещё не завершившие handshake
// сокеты (H-2, tasks/2026-11-20-security-audit.md §1). После перевода
// слушателя на собственный TlsListener (сокет в handshake не занимает
// pending-очередь) обход не нужен: совместная работа в одном event loop —
// это как раз то, что должно проверяться.
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<QTemporaryDir> g_tlsDir;
TlsManager::TlsMaterial g_tls;
QString g_tlsSkipReason;
QString g_tlsDns = QStringLiteral("proxy.test");

/** Готовит CA+лист во временном каталоге. false — тесты TLS пропускаем. */
bool setupTls()
{
    QString version;
    if (!TlsManager::opensslAvailable(&version)) {
        g_tlsSkipReason = QStringLiteral("openssl ≥ 3.0 не найден");
        return false;
    }
    g_tlsDir = std::make_unique<QTemporaryDir>();
    if (!g_tlsDir->isValid()) {
        g_tlsSkipReason = QStringLiteral("не удалось создать временный каталог");
        return false;
    }
    TlsManager::setDirectoryOverride(g_tlsDir->path());

    TlsManager tls;
    QString error;
    g_tls = tls.ensure(ProxySettings::TlsMode::Internal, {QStringLiteral("127.0.0.1")},
                       g_tlsDns, &error);
    if (!g_tls.ok) {
        g_tlsSkipReason = QStringLiteral("TlsManager: %1").arg(error);
        return false;
    }
    return true;
}

struct TlsResult {
    bool started = false;
    bool handshakeOk = false;
    int sslErrors = 0;
    int status = 0;
    qint64 firstMs = -1;
    qint64 totalMs = -1;
    int bytes = 0;
    QByteArray raw;
    bool timedOut = false;
};

/**
 * Один TLS-запрос в этом же процессе (клиент и сервер делят event loop).
 * token.isEmpty() — без авторизации; withCa=false — клиент без нашего CA.
 */
TlsResult tlsRequestInProcess(quint16 port, bool withCa, const QString &token)
{
    TlsResult result;
    QSslSocket socket;
    if (withCa) {
        QSslConfiguration config = socket.sslConfiguration();
        config.setCaCertificates({QSslCertificate::fromPath(g_tls.caPath, QSsl::Pem).value(0)});
        config.setPeerVerifyMode(QSslSocket::VerifyPeer);
        socket.setSslConfiguration(config);
    }

    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QElapsedTimer clock;
    QByteArray raw;

    QObject::connect(&socket, &QSslSocket::sslErrors, &socket,
                     [&result](const QList<QSslError> &errors) {
                         result.sslErrors += int(errors.size());
                     });
    QObject::connect(&socket, &QSslSocket::encrypted, &socket, [&] {
        result.handshakeOk = true;
        clock.start();
        QByteArray request = "GET /api/tags HTTP/1.1\r\nHost: proxy.test\r\n";
        if (!token.isEmpty())
            request += "Authorization: Bearer " + token.toUtf8() + "\r\n";
        request += "Connection: close\r\n\r\n";
        socket.write(request);
        socket.flush();
    });
    QObject::connect(&socket, &QSslSocket::readyRead, &socket, [&] {
        if (result.firstMs < 0 && clock.isValid())
            result.firstMs = clock.elapsed();
        raw += socket.readAll();
    });
    // readAll() в disconnected не вызываем: SSL-сессия там уже разрушается.
    QObject::connect(&socket, &QSslSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QSslSocket::errorOccurred, &loop,
                     [&loop](QAbstractSocket::SocketError) { loop.quit(); });
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        result.timedOut = true;
        loop.quit();
    });
    guard.start(10000);

    // Явный sslPeerName: соединение по IP, проверка по DNS-SAN листа.
    socket.connectToHostEncrypted(QStringLiteral("127.0.0.1"), port, g_tlsDns);
    loop.exec();

    result.started = socket.state() != QAbstractSocket::UnconnectedState || result.handshakeOk;
    result.raw = raw;
    result.totalMs = clock.isValid() ? clock.elapsed() : -1;
    result.bytes = raw.size();
    result.status = parseStatus(raw);
    socket.abort(); // чистое состояние для следующего последовательного запроса

    const int bodyStart = raw.indexOf("\r\n\r\n");
    if (bodyStart >= 0)
        result.raw = raw.mid(bodyStart + 4);
    return result;
}

ProxyConfig tlsConfig(const FakeUpstream &upstream)
{
    ProxyConfig cfg = baseConfig();
    cfg.listenAddresses = {QHostAddress::LocalHost};
    cfg.listenPort = 0;
    cfg.upstreamPort = upstream.port();
    cfg.tlsEnabled = true;
    cfg.tlsCertPath = g_tls.certPath;
    cfg.tlsKeyPath = g_tls.keyPath;
    return cfg;
}

void testTlsHandshake()
{
    static const char *kName = "13. TLS: handshake с нашим CA, ответ 200";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    FakeUpstream upstream(FakeUpstream::Normal);
    upstream.start();
    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    if (!proxy.start(tlsConfig(upstream), &error)) {
        report(kName, false, QStringLiteral("start: %1").arg(error));
        return;
    }

    const TlsResult r = tlsRequestInProcess(proxy.listenPort(), true, QStringLiteral("secret-token-1"));
    report(kName, r.handshakeOk && r.sslErrors == 0 && r.status == 200 && proxy.isTls(),
           QStringLiteral("handshake=%1 errors=%2 status=%3").arg(r.handshakeOk)
               .arg(r.sslErrors).arg(r.status));
    proxy.stop();
}

void testTlsAuth()
{
    static const char *kName = "14. TLS: без токена 401, с токеном 200";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    FakeUpstream upstream(FakeUpstream::Normal);
    upstream.start();
    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    if (!proxy.start(tlsConfig(upstream), &error)) {
        report(kName, false, QStringLiteral("start: %1").arg(error));
        return;
    }

    const TlsResult anonymous = tlsRequestInProcess(proxy.listenPort(), true, QString());
    const TlsResult authorized =
        tlsRequestInProcess(proxy.listenPort(), true, QStringLiteral("secret-token-1"));
    report(kName, anonymous.status == 401 && authorized.status == 200,
           QStringLiteral("401 -> %1, 200 -> %2").arg(anonymous.status).arg(authorized.status));
    proxy.stop();
}

void testTlsStreaming()
{
    static const char *kName = "15. TLS: стриминг, первый чанк раньше конца";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    FakeUpstream upstream(FakeUpstream::Stream);
    upstream.start();
    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    if (!proxy.start(tlsConfig(upstream), &error)) {
        report(kName, false, QStringLiteral("start: %1").arg(error));
        return;
    }

    const TlsResult r = tlsRequestInProcess(proxy.listenPort(), true, QStringLiteral("secret-token-1"));
    const bool bothParts = r.raw.contains("{\"part\":1}") && r.raw.contains("{\"part\":2}");
    const bool fastFirst = r.firstMs >= 0 && r.firstMs < 80;
    report(kName, r.handshakeOk && bothParts && fastFirst,
           QStringLiteral("first=%1ms total=%2ms parts=%3")
               .arg(r.firstMs).arg(r.totalMs).arg(bothParts));
    proxy.stop();
}

void testTlsUntrustedClient()
{
    static const char *kName = "16. TLS: клиент без нашего CA -> handshake обрыв";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    FakeUpstream upstream(FakeUpstream::Normal);
    upstream.start();
    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    if (!proxy.start(tlsConfig(upstream), &error)) {
        report(kName, false, QStringLiteral("start: %1").arg(error));
        return;
    }

    const TlsResult r = tlsRequestInProcess(proxy.listenPort(), false, QStringLiteral("secret-token-1"));
    report(kName, !r.handshakeOk && r.status == 0 && r.sslErrors > 0,
           QStringLiteral("handshake=%1 status=%2 errors=%3")
               .arg(r.handshakeOk).arg(r.status).arg(r.sslErrors));
    proxy.stop();
}

void testTlsCaIdempotent()
{
    static const char *kName = "17. TLS: повторный ensure не перегенерирует CA";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    const auto digest = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    };
    const QByteArray caBefore = digest(g_tls.caPath);
    const QByteArray leafBefore = digest(g_tls.certPath);

    TlsManager again;
    QString error;
    const TlsManager::TlsMaterial material =
        again.ensure(ProxySettings::TlsMode::Internal, {QStringLiteral("127.0.0.1")},
                     g_tlsDns, &error);

    const QByteArray caAfter = digest(material.caPath);
    const QByteArray leafAfter = digest(material.certPath);
    report(kName, material.ok && !caBefore.isEmpty() && caBefore == caAfter
                      && leafBefore == leafAfter,
           QStringLiteral("ca same=%1 leaf same=%2 error=%3")
               .arg(caBefore == caAfter).arg(leafBefore == leafAfter).arg(error));
}

// ---------------------------------------------------------------------------
// H-1: тело ограничивается по фактически переданным байтам + backpressure
// ---------------------------------------------------------------------------

/** RSS текущего процесса в КиБ (в тестах клиент и сервер — один процесс). */
long rssKb()
{
    QFile file(QStringLiteral("/proc/self/status"));
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    for (const QByteArray &line : file.readAll().split('\n')) {
        if (line.startsWith("VmRSS:"))
            return line.simplified().split(' ').value(1).toLong();
    }
    return -1;
}

struct FloodResult {
    int status = 0;          // -1 — прокси не стартовал
    qint64 sent = 0;
    bool closed = false;
    bool stalled = false;    // клиент перестал помещаться в сокет (backpressure)
    long rssGrowthKb = 0;
    QByteArray response;
};

/**
 * Льёт тело в прокси, пока он не ответит/не закроет соединение или пока не
 * будет достигнут target. head — уже готовый блок заголовков с \r\n\r\n.
 */
FloodResult floodBody(FakeUpstream::Mode mode, const ProxyConfig &base, const QByteArray &head,
                      const QByteArray &frame, qint64 target, int timeoutMs)
{
    FloodResult out;
    FakeUpstream upstream(mode);
    if (!upstream.start()) {
        out.status = -1;
        return out;
    }
    ProxyConfig cfg = base;
    cfg.listenAddresses = {QHostAddress::LocalHost};
    cfg.listenPort = 0;
    cfg.upstreamPort = upstream.port();

    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    if (!proxy.start(cfg, &error)) {
        out.status = -1;
        return out;
    }

    const long before = rssKb();
    QTcpSocket socket;
    QElapsedTimer clock;
    clock.start();
    QObject::connect(&socket, &QTcpSocket::readyRead, &socket,
                     [&out, &socket] { out.response += socket.readAll(); });
    socket.connectToHost(QHostAddress::LocalHost, proxy.listenPort());
    if (!socket.waitForConnected(2000)) {
        out.status = -1;
        return out;
    }
    socket.write(head);
    socket.waitForBytesWritten(2000);

    int stalls = 0;
    while (out.sent < target && clock.elapsed() < timeoutMs) {
        const qint64 n = socket.write(frame);
        if (n <= 0)
            break;
        out.sent += n;
        if (socket.waitForBytesWritten(150))
            stalls = 0;
        else
            ++stalls;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (!out.response.isEmpty())
            break;
        if (socket.state() == QAbstractSocket::UnconnectedState) {
            out.closed = true;
            break;
        }
        if (stalls >= 3) {
            out.stalled = true;
            break;
        }
    }

    // Дать прокси время ответить (413/504) или закрыть соединение.
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 3000 && out.response.isEmpty()
           && socket.state() != QAbstractSocket::UnconnectedState) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    out.response += socket.readAll();
    if (socket.state() == QAbstractSocket::UnconnectedState)
        out.closed = true;
    out.status = parseStatus(out.response);
    out.rssGrowthKb = rssKb() - before;

    socket.abort();
    proxy.stop();
    return out;
}

void testBodyLimitActualBytes()
{
    ProxyConfig cfg = baseConfig();
    const qint64 limit = 2 * 1024 * 1024;
    cfg.maxBodyBytes = limit;

    const QByteArray chunk(64 * 1024, 'a');
    const QByteArray frame = QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n";
    QByteArray head = "POST /api/chat HTTP/1.1\r\nHost: proxy.test\r\n"
                      + bearer("secret-token-1") + "\r\nTransfer-Encoding: chunked\r\n\r\n";

    // Апстрим не читает: лимит обязан сработать по счётчику, а не по буферу.
    const FloodResult r = floodBody(FakeUpstream::Blackhole, cfg, head, frame,
                                    limit * 2 + 1024 * 1024, 10000);
    const bool stopped = r.status == 413 || (r.closed && r.status == 0);
    report("18. chunked-тело 2× лимита -> 413/закрытие, RSS не растёт",
           stopped && r.sent < limit + 1024 * 1024 && r.rssGrowthKb < 16 * 1024,
           QStringLiteral("status=%1 sent=%2KiB rss+%3KiB closed=%4")
               .arg(r.status).arg(r.sent / 1024).arg(r.rssGrowthKb).arg(r.closed));
}

void testBodyLimitLyingContentLength()
{
    ProxyConfig cfg = baseConfig();
    const qint64 limit = 2 * 1024 * 1024;
    cfg.maxBodyBytes = limit;

    const QByteArray chunk(64 * 1024, 'a');
    QByteArray head = "POST /api/chat HTTP/1.1\r\nHost: proxy.test\r\n"
                      + bearer("secret-token-1") + "\r\nContent-Length: 10\r\n\r\n";

    const FloodResult r = floodBody(FakeUpstream::Blackhole, cfg, head, chunk,
                                    limit * 2 + 1024 * 1024, 10000);
    const bool stopped = r.status == 413 || (r.closed && r.status == 0);
    report("19. маленький Content-Length с большим телом -> 413/закрытие",
           stopped && r.sent < limit + 1024 * 1024 && r.rssGrowthKb < 16 * 1024,
           QStringLiteral("status=%1 sent=%2KiB rss+%3KiB")
               .arg(r.status).arg(r.sent / 1024).arg(r.rssGrowthKb));
}

void testBackpressureBounded()
{
    ProxyConfig cfg = baseConfig();
    cfg.maxBodyBytes = 64LL * 1024 * 1024;

    FakeUpstream upstream(FakeUpstream::Blackhole);
    upstream.start();
    ProxyConfig listen = cfg;
    listen.listenAddresses = {QHostAddress::LocalHost};
    listen.listenPort = 0;
    listen.upstreamPort = upstream.port();

    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    if (!proxy.start(listen, &error)) {
        report("20. backpressure: RSS ограничен, прокси жив", false, error);
        return;
    }

    const long before = rssKb();
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, proxy.listenPort());
    if (!socket.waitForConnected(2000)) {
        report("20. backpressure: RSS ограничен, прокси жив", false,
               QStringLiteral("connect failed"));
        proxy.stop();
        return;
    }
    socket.write("POST /api/chat HTTP/1.1\r\nHost: proxy.test\r\n"
                 "Authorization: Bearer secret-token-1\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n");
    socket.waitForBytesWritten(2000);

    const QByteArray chunk(64 * 1024, 'a');
    const QByteArray frame = QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n";
    qint64 sent = 0;
    int stalls = 0;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 8000 && stalls < 3 && sent < 40LL * 1024 * 1024) {
        const qint64 n = socket.write(frame);
        if (n <= 0)
            break;
        sent += n;
        if (socket.waitForBytesWritten(150))
            stalls = 0;
        else
            ++stalls;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    const long growth = rssKb() - before;

    // Прокси должен остаться живым: запрос без токена получает 401 без апстрима.
    const Response probe = sendRequest(QHostAddress::LocalHost, proxy.listenPort(),
                                       getRequest(QByteArrayLiteral("/api/tags")));
    report("20. backpressure: RSS ограничен, прокси жив",
           stalls >= 3 && sent < 40LL * 1024 * 1024 && growth < 24 * 1024
               && probe.status == 401,
           QStringLiteral("sent=%1KiB stall=%2 rss+%3KiB probe=%4")
               .arg(sent / 1024).arg(stalls).arg(growth).arg(probe.status));
    socket.abort();
    proxy.stop();
}

// ---------------------------------------------------------------------------
// H-3: абсолютный таймаут на заголовки
// ---------------------------------------------------------------------------

void testHeaderTimeout()
{
    ProxyConfig cfg = baseConfig();
    cfg.headerTimeoutMs = 300;
    cfg.maxConnections = 2;

    Fixture f(FakeUpstream::Normal, cfg, sampleTokens());
    if (!f.started) {
        report("21. заголовки не пришли -> 408 за headerTimeout, слот свободен", false, f.error);
        return;
    }

    QTcpSocket silent;
    silent.connectToHost(QHostAddress::LocalHost, f.proxy.listenPort());
    if (!silent.waitForConnected(2000)) {
        report("21. заголовки не пришли -> 408 за headerTimeout, слот свободен", false,
               QStringLiteral("connect failed"));
        return;
    }

    QByteArray response;
    QElapsedTimer clock;
    clock.start();
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&silent, &QTcpSocket::readyRead, &silent,
                     [&response, &silent] { response += silent.readAll(); });
    QObject::connect(&silent, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);

    // Капаем по байту: таймаут абсолютный и не должен продлеваться (H-3).
    QTimer drip;
    drip.setInterval(100);
    int dripped = 0;
    QObject::connect(&drip, &QTimer::timeout, [&silent, &dripped] {
        if (silent.write("X") > 0)
            ++dripped;
    });
    drip.start();
    guard.start(3000);
    loop.exec();
    drip.stop();
    const qint64 elapsed = clock.elapsed();

    // Слот освободился: следующий запрос обслуживается.
    const Response ok = f.get(QByteArrayLiteral("/api/tags"), {bearer("secret-token-1")});
    report("21. заголовки не пришли -> 408 за headerTimeout, слот свободен",
           (parseStatus(response) == 408 || silent.state() == QAbstractSocket::UnconnectedState)
               && elapsed < 2500 && dripped >= 2 && ok.status == 200,
           QStringLiteral("status=%1 elapsed=%2ms dripped=%3 next=%4")
               .arg(parseStatus(response)).arg(elapsed).arg(dripped).arg(ok.status));
}

// ---------------------------------------------------------------------------
// M-6: таймауты апстрима
// ---------------------------------------------------------------------------

void testUpstreamReadTimeout()
{
    ProxyConfig cfg = baseConfig();
    cfg.upstreamTimeoutMs = 300;

    Fixture f(FakeUpstream::Silent, cfg, sampleTokens());
    if (!f.started) {
        report("22. апстрим молчит -> 504", false, f.error);
        return;
    }

    QElapsedTimer clock;
    clock.start();
    const Response r = f.get(QByteArrayLiteral("/api/tags"), {bearer("secret-token-1")});
    report("22. апстрим молчит -> 504", r.status == 504,
           QStringLiteral("status=%1 elapsed=%2ms").arg(r.status).arg(clock.elapsed()));
}

void testUpstreamWriteTimeout()
{
    ProxyConfig cfg = baseConfig();
    cfg.maxBodyBytes = 64LL * 1024 * 1024;
    cfg.upstreamWriteTimeoutMs = 400;

    const QByteArray chunk(64 * 1024, 'a');
    const QByteArray frame = QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n";
    QByteArray head = "POST /api/chat HTTP/1.1\r\nHost: proxy.test\r\n"
                      + bearer("secret-token-1") + "\r\nTransfer-Encoding: chunked\r\n\r\n";

    const FloodResult r = floodBody(FakeUpstream::Blackhole, cfg, head, frame,
                                    32LL * 1024 * 1024, 10000);
    // Backpressure останавливает клиента на 4 МиБ буфера апстрима, затем
    // write-таймаут рвёт соединение (504 не всегда доходит — клиент ещё пишет).
    report("23. апстрим не забирает данные -> закрытие по write-таймауту",
           (r.closed || r.status == 504) && r.sent > 0,
           QStringLiteral("status=%1 sent=%2KiB closed=%3")
               .arg(r.status).arg(r.sent / 1024).arg(r.closed));
}

// ---------------------------------------------------------------------------
// M-6: медленный, но прогрессирующий апстрим не должен давать ложный 504
// ---------------------------------------------------------------------------

void testUpstreamWriteProgress()
{
    static const char *kName = "23b. медленный, но читающий апстрим -> 200, не ложный 504";

    // Апстрим забирает данные маленькими порциями: буфер прокси долго держится
    // выше low-water, но прогресс есть на каждом шаге.
    QTcpServer upstream;
    if (!upstream.listen(QHostAddress::LocalHost, 0)) {
        report(kName, false, QStringLiteral("upstream listen failed"));
        return;
    }
    QObject::connect(&upstream, &QTcpServer::newConnection, &upstream, [&upstream] {
        while (QTcpSocket *socket = upstream.nextPendingConnection()) {
            socket->setReadBufferSize(64 * 1024);
            auto *state = new QByteArray;
            QObject::connect(socket, &QObject::destroyed, [state] { delete state; });
            auto *timer = new QTimer(socket);
            timer->setInterval(16);
            QObject::connect(timer, &QTimer::timeout, socket, [socket, state] {
                if (socket->bytesAvailable() <= 0)
                    return;
                const QByteArray data = socket->read(256 * 1024);
                state->append(data);
                if (state->endsWith("\r\n0\r\n\r\n")) {
                    state->clear();
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                                  "Content-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}");
                    socket->disconnectFromHost();
                }
            });
            timer->start();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    ProxyConfig cfg = baseConfig();
    cfg.maxBodyBytes = 64LL * 1024 * 1024;
    cfg.upstreamWriteTimeoutMs = 300; // меньше полного времени передачи, больше пауз прогресса
    cfg.listenAddresses = {QHostAddress::LocalHost};
    cfg.listenPort = 0;
    cfg.upstreamPort = upstream.serverPort();

    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    if (!proxy.start(cfg, &error)) {
        report(kName, false, error);
        return;
    }

    QTcpSocket client;
    QByteArray raw;
    QObject::connect(&client, &QTcpSocket::readyRead, &client,
                     [&client, &raw] { raw += client.readAll(); });
    client.connectToHost(QHostAddress::LocalHost, proxy.listenPort());
    if (!client.waitForConnected(2000)) {
        report(kName, false, QStringLiteral("connect failed"));
        proxy.stop();
        return;
    }
    client.write("POST /api/chat HTTP/1.1\r\nHost: proxy.test\r\n"
                 "Authorization: Bearer secret-token-1\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n");
    client.waitForBytesWritten(1000);

    const QByteArray chunk(64 * 1024, 'a');
    const QByteArray frame = QByteArray::number(chunk.size(), 16) + "\r\n" + chunk + "\r\n";
    QElapsedTimer clock;
    clock.start();
    qint64 sent = 0;
    const qint64 target = 12LL * 1024 * 1024;

    // Пишем без блокирующих waitFor*: иначе таймеры апстрима и прокси
    // обслуживаются неравномерно и тест меряет не то. Кормим, пока у клиента
    // есть место в буфере, и ждём ответа в обычном event loop.
    QTimer feeder;
    feeder.setInterval(2);
    bool terminatorSent = false;
    QObject::connect(&feeder, &QTimer::timeout, &client, [&] {
        if (sent >= target) {
            if (terminatorSent)
                return;
            terminatorSent = true;
            feeder.stop();
            client.write("0\r\n\r\n");
            client.flush();
            return;
        }
        if (client.bytesToWrite() > 2LL * 1024 * 1024)
            return; // не копим тело у себя — пусть работает backpressure
        const qint64 n = client.write(frame);
        if (n > 0)
            sent += n;
    });
    feeder.start();

    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&client, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&client, &QTcpSocket::errorOccurred, &loop, &QEventLoop::quit);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(10000);
    loop.exec();
    feeder.stop();

    raw += client.readAll();
    const int status = parseStatus(raw);
    const int bodyAt = raw.indexOf("\r\n\r\n");
    const QByteArray body = bodyAt >= 0 ? raw.mid(bodyAt + 4) : QByteArray();

    report(kName, status == 200 && raw.contains("{\"ok\":true}") && sent > 0,
           QStringLiteral("status=%1 sent=%2KiB elapsed=%3ms body=%4")
               .arg(status).arg(sent / 1024).arg(clock.elapsed())
               .arg(QString::fromLatin1(body.left(120))));
    client.abort();
    proxy.stop();
}


// ---------------------------------------------------------------------------
// H-2: лимит считает соединения до handshake; висящие TCP не блокируют TLS
// ---------------------------------------------------------------------------

void testTlsPendingThreshold()
{
    static const char *kName =
        "24. висящие TCP: легитимный 200 и бюджет ограничивает слоты до handshake";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    FakeUpstream upstream(FakeUpstream::Normal);
    upstream.start();
    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;

    // Часть A (регресс исходного бага): 30 «молчащих» TCP при большом лимите
    // не должны занимать pending-очередь и мешать легитимному handshake.
    ProxyConfig cfg = tlsConfig(upstream);
    cfg.maxConnections = 256;
    if (!proxy.start(cfg, &error)) {
        report(kName, false, QStringLiteral("start: %1").arg(error));
        return;
    }

    std::vector<std::unique_ptr<QTcpSocket>> stalled;
    for (int i = 0; i < 30; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        socket->connectToHost(QHostAddress::LocalHost, proxy.listenPort());
        if (!socket->waitForConnected(2000))
            break;
        stalled.push_back(std::move(socket));
    }
    QElapsedTimer warm;
    warm.start();
    while (warm.elapsed() < 400)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    const TlsResult legit =
        tlsRequestInProcess(proxy.listenPort(), true, QStringLiteral("secret-token-1"));
    const bool hangingDoNotBlock = stalled.size() == 30 && legit.status == 200;
    const qsizetype stalledCount = stalled.size();

    for (auto &socket : stalled)
        socket->abort();
    stalled.clear();
    warm.restart();
    while (warm.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    // Часть B (мутация «бюджет отключён»): лимит обязан ограничивать число
    // одновременно учтённых сокетов ещё до handshake. 40 молчащих TCP при
    // maxConnections=8 — в бюджете не больше 8, иначе это утечка/обход лимита.
    ProxyConfig small = tlsConfig(upstream);
    small.maxConnections = 8;
    if (!proxy.start(small, &error)) {
        report(kName, false, QStringLiteral("restart: %1").arg(error));
        return;
    }

    std::vector<std::unique_ptr<QTcpSocket>> flood;
    for (int i = 0; i < 40; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        socket->connectToHost(QHostAddress::LocalHost, proxy.listenPort());
        socket->waitForConnected(1000);
        flood.push_back(std::move(socket));
    }
    warm.restart();
    while (warm.elapsed() < 500)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const int liveDuring = proxy.activeConnections();
    const bool budgetBounded = liveDuring > 0 && liveDuring <= small.maxConnections;

    for (auto &socket : flood)
        socket->abort();
    flood.clear();

    // Слоты освобождаются, бюджет возвращается к 0, следующий клиент — 200.
    TlsResult after;
    bool afterOk = false;
    for (int attempt = 0; attempt < 40; ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (afterOk) {
            if (proxy.activeConnections() == 0)
                break;
            continue;
        }
        if (proxy.activeConnections() != 0)
            continue;
        after = tlsRequestInProcess(proxy.listenPort(), true, QStringLiteral("secret-token-1"));
        afterOk = after.status == 200;
    }
    report(kName,
           hangingDoNotBlock && budgetBounded && afterOk && proxy.activeConnections() == 0,
           QStringLiteral("stalled=%1 legit=%2 budget=%3/8 after=%4 budgetAfter=%5")
               .arg(stalledCount).arg(legit.status).arg(liveDuring)
               .arg(after.status).arg(proxy.activeConnections()));
    proxy.stop();
}

void testTlsSuccessfulHandshakeBudgetLeak()
{
    // Регресс H-2: успешный handshake раньше снимал связь destroyed→release
    // (socket->disconnect(this)), и после maxConnections успешных запросов
    // прокси переставал принимать TLS-клиентов. 5 запросов при лимите 2.
    static const char *kName =
        "25b. успешный TLS-handshake возвращает слот (5/5 при maxConnections=2)";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    FakeUpstream upstream(FakeUpstream::Normal);
    upstream.start();
    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    ProxyConfig cfg = tlsConfig(upstream);
    cfg.maxConnections = 2;
    QString error;
    if (!proxy.start(cfg, &error)) {
        report(kName, false, QStringLiteral("start: %1").arg(error));
        return;
    }

    int ok = 0;
    QString statuses;
    for (int i = 0; i < 5; ++i) {
        const TlsResult r =
            tlsRequestInProcess(proxy.listenPort(), true, QStringLiteral("secret-token-1"));
        if (r.status == 200)
            ++ok;
        statuses += QString::number(r.status) + (i < 4 ? ',' : ' ');
    }
    // deleteLater() после закрытия каждого соединения.
    for (int i = 0; i < 20; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    report(kName, ok == 5 && proxy.activeConnections() == 0,
           QStringLiteral("statuses=%1 budget=%2").arg(statuses).arg(proxy.activeConnections()));
    proxy.stop();
}

void testTlsPreHandshakeBudget()
{
    static const char *kName = "25. maxConnections учитывает TLS-соединения до handshake";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    FakeUpstream upstream(FakeUpstream::Normal);
    upstream.start();
    ProxyConfig cfg = tlsConfig(upstream);
    cfg.maxConnections = 2;

    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    if (!proxy.start(cfg, &error)) {
        report(kName, false, QStringLiteral("start: %1").arg(error));
        return;
    }

    std::vector<std::unique_ptr<QTcpSocket>> stalled;
    for (int i = 0; i < 2; ++i) {
        auto socket = std::make_unique<QTcpSocket>();
        socket->connectToHost(QHostAddress::LocalHost, proxy.listenPort());
        if (!socket->waitForConnected(2000))
            break;
        stalled.push_back(std::move(socket));
    }
    QElapsedTimer warm;
    warm.start();
    while (warm.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    // Бюджет занят двумя молчащими сокетами: третий не должен обслуживаться.
    const TlsResult blocked = tlsRequestInProcess(proxy.listenPort(), true, QStringLiteral("secret-token-1"));
    const bool rejected = !blocked.handshakeOk && blocked.status == 0;

    // Освобождаем бюджет — он не должен «протечь».
    for (auto &socket : stalled)
        socket->abort();
    stalled.clear();

    TlsResult after;
    for (int attempt = 0; attempt < 20; ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        after = tlsRequestInProcess(proxy.listenPort(), true, QStringLiteral("secret-token-1"));
        if (after.status == 200)
            break;
    }
    report(kName, stalled.empty() && rejected && after.status == 200,
           QStringLiteral("blocked handshake=%1 status=%2 after=%3")
               .arg(blocked.handshakeOk).arg(blocked.status).arg(after.status));
    proxy.stop();
}

void testSequentialTlsInProcess()
{
    static const char *kName = "26. TLS: 10 последовательных in-process запросов";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    FakeUpstream upstream(FakeUpstream::Normal);
    upstream.start();
    OllamaProxy proxy;
    proxy.setTokenMap(sampleTokens());
    QString error;
    if (!proxy.start(tlsConfig(upstream), &error)) {
        report(kName, false, QStringLiteral("start: %1").arg(error));
        return;
    }

    int ok = 0;
    int badHandshake = 0;
    for (int i = 0; i < 10; ++i) {
        const TlsResult r = tlsRequestInProcess(proxy.listenPort(), true,
                                                QStringLiteral("secret-token-1"));
        if (r.status == 200)
            ++ok;
        if (!r.handshakeOk)
            ++badHandshake;
    }
    report(kName, ok == 10 && badHandshake == 0,
           QStringLiteral("ok=%1 badHandshake=%2").arg(ok).arg(badHandshake));
    proxy.stop();
}

// ---------------------------------------------------------------------------
// M-5 / L-2: продление сертификата и симлинки
// ---------------------------------------------------------------------------

void testTlsRenewKeepsFiles()
{
    static const char *kName = "27. invalidateLeaf не удаляет файлы, ensure перевыпускает лист";
    if (!g_tls.ok) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    const auto digest = [](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return QByteArray();
        return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
    };
    const QByteArray certBefore = digest(g_tls.certPath);
    const QByteArray keyBefore = digest(g_tls.keyPath);

    TlsManager tls;
    tls.invalidateLeaf();
    // Старая пара должна остаться на диске до успешной генерации новой (M-5).
    const bool filesKept = QFile::exists(g_tls.certPath) && QFile::exists(g_tls.keyPath);

    QString error;
    const TlsManager::TlsMaterial material =
        tls.ensure(ProxySettings::TlsMode::Internal, {QStringLiteral("127.0.0.1")},
                   g_tlsDns, &error);
    const bool renewed = material.ok && digest(material.certPath) != certBefore
                         && digest(material.keyPath) != keyBefore;
    if (material.ok)
        g_tls = material;

    report(kName, material.ok && filesKept && renewed,
           QStringLiteral("ok=%1 kept=%2 renewed=%3 error=%4")
               .arg(material.ok).arg(filesKept).arg(renewed).arg(error));
}

void testTlsSymlinkRefused()
{
    static const char *kName = "28. symlink на ca.key -> отказ";
    if (!g_tls.ok || !g_tlsDir || !g_tlsDir->isValid()) {
        skip(kName, g_tlsSkipReason);
        return;
    }

    QTemporaryDir dir;
    if (!dir.isValid()) {
        skip(kName, QStringLiteral("временный каталог недоступен"));
        return;
    }
    const QString target = dir.filePath(QStringLiteral("target.key"));
    {
        QFile file(target);
        if (!file.open(QIODevice::WriteOnly))
            return;
        file.write("not a key");
    }
    if (!QFile::link(target, dir.filePath(QStringLiteral("ca.key")))) {
        skip(kName, QStringLiteral("симлинки не поддерживаются"));
        return;
    }

    TlsManager::setDirectoryOverride(dir.path());
    TlsManager tls;
    QString error;
    const TlsManager::TlsMaterial material =
        tls.ensure(ProxySettings::TlsMode::Internal, {QStringLiteral("127.0.0.1")},
                   QStringLiteral("proxy.test"), &error);
    TlsManager::setDirectoryOverride(g_tlsDir->path());

    report(kName, !material.ok && error.contains(QStringLiteral("символическ")),
           QStringLiteral("ok=%1 error=%2").arg(material.ok).arg(error));
}

// ---------------------------------------------------------------------------
// M-3: имена устройств из кошелька
// ---------------------------------------------------------------------------

void testDeviceNameValidation()
{
    const bool good = isValidDeviceName(QStringLiteral("laptop-1.dev_x"));
    const bool badCrlf = !isValidDeviceName(QStringLiteral("x\r\nX-Admin: 1"));
    const bool badSpace = !isValidDeviceName(QStringLiteral("two words"));
    const bool badLong = !isValidDeviceName(QString(33, QLatin1Char('a')));
    const bool badEmpty = !isValidDeviceName(QString());

    TokenMap map;
    map.setTokens({{QByteArrayLiteral("t1"), QStringLiteral("laptop")},
                   {QByteArrayLiteral("t2"), QStringLiteral("x\r\nX-Admin: 1")},
                   {QByteArrayLiteral("t3"), QString()},
                   {QByteArrayLiteral("t4"), QString(33, QLatin1Char('a'))},
                   {QByteArray(), QStringLiteral("empty-token")}});
    const bool onlyValid = map.size() == 1
                           && map.deviceForToken(QByteArrayLiteral("t1")) == QStringLiteral("laptop")
                           && map.deviceForToken(QByteArrayLiteral("t2")).isEmpty();

    report("29. невалидные имена устройств отбрасываются (кошелёк/TokenMap)",
           good && badCrlf && badSpace && badLong && badEmpty && onlyValid,
           QStringLiteral("good=%1 crlf=%2 size=%3").arg(good).arg(badCrlf).arg(map.size()));
}

// ---------------------------------------------------------------------------
// M-1 / M-2 / M-4 / L-1 / L-8
// ---------------------------------------------------------------------------

void testHopByHopHeaders()
{
    Fixture f(FakeUpstream::EchoHeaders, baseConfig(), sampleTokens());
    const Response r = f.get(QByteArrayLiteral("/api/tags"),
                             {bearer("secret-token-1"),
                              QByteArrayLiteral("Proxy-Authorization: Basic Zm9v"),
                              QByteArrayLiteral("TE: trailers"),
                              QByteArrayLiteral("Trailer: X-Foo"),
                              QByteArrayLiteral("Upgrade: websocket"),
                              QByteArrayLiteral("Proxy-Authenticate: Basic"),
                              QByteArrayLiteral("Connection: X-Foo, keep-alive"),
                              QByteArrayLiteral("X-Foo: secret"),
                              QByteArrayLiteral("X-Keep-Me: yes")});
    const QByteArray seen = r.body;
    const bool noLeak = !seen.contains("Proxy-Authorization") && !seen.contains("Proxy-Authenticate")
                        && !seen.contains("\r\nTE:") && !seen.contains("\r\nTrailer:")
                        && !seen.contains("\r\nUpgrade:") && !seen.contains("\r\nX-Foo:")
                        && !seen.contains("Connection: X-Foo");
    const bool kept = seen.contains("X-Keep-Me: yes");
    report("30. hop-by-hop заголовки не доходят до апстрима", f.started && r.status == 200
                                                                   && noLeak && kept,
           QStringLiteral("status=%1 leak=%2 kept=%3")
               .arg(r.status).arg(!noLeak).arg(kept));
}

void testStrictContentLength()
{
    const auto send = [](const QByteArray &extra) {
        Fixture f(FakeUpstream::EchoBody, baseConfig(), sampleTokens());
        QByteArray request = "POST /api/echo HTTP/1.1\r\nHost: proxy.test\r\n"
                             + bearer("secret-token-1") + "\r\n" + extra + "\r\n\r\n";
        return sendRequest(QHostAddress::LocalHost, f.proxy.listenPort(), request).status;
    };

    const int duplicate = send("Content-Length: 5\r\nContent-Length: 5");
    const int both = send("Transfer-Encoding: chunked\r\nContent-Length: 5");
    const int negative = send("Content-Length: -1");
    const int plus = send("Content-Length: +1");
    const int exponent = send("Content-Length: 1e9");
    const int junk = send("Content-Length: 5abc");

    report("31. Content-Length: дубликат/TE+CL/мусор -> 400",
           duplicate == 400 && both == 400 && negative == 400 && plus == 400
               && exponent == 400 && junk == 400,
           QStringLiteral("dup=%1 both=%2 neg=%3 plus=%4 exp=%5 junk=%6")
               .arg(duplicate).arg(both).arg(negative).arg(plus).arg(exponent).arg(junk));
}

void testParserTable()
{
    const auto makeHead = [](const QByteArray &requestLine,
                             const QList<QByteArray> &headers = {}) {
        QByteArray head = requestLine + "\r\n";
        for (const QByteArray &header : headers)
            head += header + "\r\n";
        return head + "\r\n";
    };

    struct Case {
        const char *name;
        QByteArray head;
        int expected;
        bool chunked = false;
    };

    QList<Case> cases;
    cases.append({"valid", makeHead("GET /api/tags HTTP/1.1", {"Host: x"}), 0});
    cases.append({"nul-value", makeHead("GET / HTTP/1.1", {QByteArray("X-N: a") + '\0' + "b"}),
                  400});
    cases.append({"nul-path", QByteArray("GET /a") + '\0' + "b HTTP/1.1\r\n\r\n", 400});
    cases.append({"cr-path", makeHead("GET /a\rb HTTP/1.1"), 400});
    cases.append({"cr-in-value", makeHead("GET / HTTP/1.1", {"X-A: v\rInjected: 1"}), 400});
    cases.append({"obs-fold", makeHead("GET / HTTP/1.1", {"X-Long: a", "  b"}), 400});
    cases.append({"empty-name", makeHead("GET / HTTP/1.1", {"   : value"}), 400});
    cases.append({"space-in-name", makeHead("GET / HTTP/1.1", {"X Y: v"}), 400});
    cases.append({"no-colon", makeHead("GET / HTTP/1.1", {"X-No-Colon"}), 400});
    cases.append({"8bit-value", makeHead("GET / HTTP/1.1", {QByteArray("X-N: ") + "\xff\xfe"}),
                  400});
    cases.append({"del-value", makeHead("GET / HTTP/1.1", {QByteArray("X-N: a") + '\x7f'}),
                  400});
    cases.append({"dup-cl-same", makeHead("GET / HTTP/1.1",
                                          {"Content-Length: 5", "Content-Length: 5"}),
                  400});
    cases.append({"dup-cl-diff", makeHead("GET / HTTP/1.1",
                                          {"Content-Length: 5", "Content-Length: 6"}),
                  400});
    cases.append({"te-cl", makeHead("GET / HTTP/1.1",
                                    {"Transfer-Encoding: chunked", "Content-Length: 5"}),
                  400});
    // TE: chunked — только последний токен из списка, регистр не важен,
    // подстрока "chunked" внутри другого токена не считается (M-2).
    cases.append({"te-chunked", makeHead("GET / HTTP/1.1", {"Transfer-Encoding: chunked"}),
                  0, true});
    cases.append({"te-chunked-upper", makeHead("GET / HTTP/1.1",
                                               {"Transfer-Encoding: CHUNKED"}),
                  0, true});
    cases.append({"te-gzip-chunked", makeHead("GET / HTTP/1.1",
                                              {"Transfer-Encoding: gzip, chunked"}),
                  0, true});
    cases.append({"te-chunked-not-last", makeHead("GET / HTTP/1.1",
                                                  {"Transfer-Encoding: chunked, gzip"}),
                  400});
    cases.append({"te-substring", makeHead("GET / HTTP/1.1",
                                           {"Transfer-Encoding: notchunked"}),
                  400});
    cases.append({"te-param", makeHead("GET / HTTP/1.1",
                                       {"Transfer-Encoding: chunked;foo=1"}),
                  400});
    cases.append({"connection-token", makeHead("GET / HTTP/1.1",
                                               {"Connection: X-Foo", "X-Foo: v"}),
                  0});
    cases.append({"connection-bad-token", makeHead("GET / HTTP/1.1",
                                                   {"Connection: X Foo"}),
                  400});
    cases.append({"cl-negative", makeHead("GET / HTTP/1.1", {"Content-Length: -1"}), 400});
    cases.append({"cl-plus", makeHead("GET / HTTP/1.1", {"Content-Length: +1"}), 400});
    cases.append({"cl-exp", makeHead("GET / HTTP/1.1", {"Content-Length: 1e9"}), 400});
    cases.append({"cl-junk", makeHead("GET / HTTP/1.1", {"Content-Length: 5abc"}), 400});
    cases.append({"cl-overflow", makeHead("GET / HTTP/1.1",
                                          {"Content-Length: 99999999999999999999"}),
                  400});
    cases.append({"cl-too-big", makeHead("GET / HTTP/1.1", {"Content-Length: 200000000"}),
                  413});
    cases.append({"bad-version", makeHead("GET / HTTP/9.9"), 400});
    cases.append({"extra-token", makeHead("GET / HTTP/1.1 x"), 400});
    cases.append({"bad-method", makeHead("G@T / HTTP/1.1"), 400});
    cases.append({"method-long", makeHead(QByteArray(33, 'M') + " / HTTP/1.1"), 501});
    cases.append({"uri-long", makeHead("GET /" + QByteArray(8192, 'a') + " HTTP/1.1"), 414});
    cases.append({"lf-only", QByteArray("GET / HTTP/1.1\nHost: x\n\n"), 0});

    QList<QByteArray> many;
    for (int i = 0; i < 101; ++i)
        many.append("X-H" + QByteArray::number(i) + ": v");
    cases.append({"too-many-headers", makeHead("GET / HTTP/1.1", many), 431});

    int failed = 0;
    QString details;
    for (const Case &c : cases) {
        const ProxyHeadRequest parsed = parseProxyHead(c.head, 100LL * 1024 * 1024);
        bool ok = parsed.errorStatus == c.expected;
        if (ok && c.expected != 0)
            ok = parsed.headers.isEmpty() && parsed.method.isEmpty() && parsed.path.isEmpty();
        if (ok && c.expected == 0)
            ok = parsed.chunked == c.chunked;
        if (!ok) {
            ++failed;
            details += QStringLiteral("%1: got %2 expected %3; ")
                           .arg(QString::fromLatin1(c.name))
                           .arg(parsed.errorStatus)
                           .arg(c.expected);
        }
    }
    report("32. таблица парсера: control-байты/obs-fold/CL/лимиты", failed == 0,
           failed == 0 ? QString() : details);
}

void testUpstreamHostValidation()
{
    const bool valid = isValidUpstreamHost(QStringLiteral("127.0.0.1"))
                       && isValidUpstreamHost(QStringLiteral("::1"))
                       && isValidUpstreamHost(QStringLiteral("localhost"))
                       && isValidUpstreamHost(QStringLiteral("ollama.local"));
    const bool invalid = !isValidUpstreamHost(QStringLiteral("127.0.0.1\r\nX: 1"))
                         && !isValidUpstreamHost(QStringLiteral("bad host"))
                         && !isValidUpstreamHost(QStringLiteral("host:11434"))
                         && !isValidUpstreamHost(QString());

    ProxyConfig cfg = baseConfig();
    cfg.upstreamHost = QStringLiteral("evil\r\nX-Admin: 1");
    OllamaProxy proxy;
    QString error;
    const bool refused = !proxy.start(cfg, &error);

    report("33. upstreamHost валидируется (L-8)", valid && invalid && refused, error);
}

void testAdminApiRestriction()
{
    // Чистая функция: точное совпадение пути, всё «хитрое» — отказ.
    const bool clientOk = isClientApiRequest("POST", "/api/chat")
                          && isClientApiRequest("POST", "/api/generate?stream=false")
                          && isClientApiRequest("GET", "/api/tags")
                          && isClientApiRequest("HEAD", "/")
                          && isClientApiRequest("POST", "/v1/chat/completions")
                          && isClientApiRequest("GET", "/v1/models/qwen3:8b");
    const bool adminDenied = !isClientApiRequest("DELETE", "/api/delete")
                             && !isClientApiRequest("POST", "/api/pull")
                             && !isClientApiRequest("POST", "/api/create")
                             && !isClientApiRequest("POST", "/api/copy")
                             && !isClientApiRequest("POST", "/api/blobs/sha256:00")
                             && !isClientApiRequest("GET", "/api/chat")
                             && !isClientApiRequest("POST", "/api/%63hat")
                             && !isClientApiRequest("POST", "/api/chat/../delete")
                             && !isClientApiRequest("GET", "/v1/models/a/../../api/delete")
                             && !isClientApiRequest("GET", "/v1/models/a%2F..");

    // laptop — admin, phone — нет.
    TokenMap tokens;
    tokens.setTokens({{QByteArrayLiteral("secret-token-1"), QStringLiteral("laptop")},
                      {QByteArrayLiteral("secret-token-2"), QStringLiteral("phone")}},
                     {QStringLiteral("laptop")});
    const auto request = [](const char *method, const char *path, const char *token) {
        return QByteArray(method) + ' ' + path + " HTTP/1.1\r\nHost: proxy.test\r\n"
               + bearer(token) + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    };

    ProxyConfig restricted = baseConfig();
    restricted.restrictAdminApi = true;
    Fixture f(FakeUpstream::Normal, restricted, tokens);
    const Response phoneTags = f.get(QByteArrayLiteral("/api/tags"), {bearer("secret-token-2")});
    const Response phoneDelete = sendRequest(QHostAddress::LocalHost, f.proxy.listenPort(),
                                             request("DELETE", "/api/delete", "secret-token-2"));
    const Response laptopDelete = sendRequest(QHostAddress::LocalHost, f.proxy.listenPort(),
                                              request("DELETE", "/api/delete", "secret-token-1"));

    // По умолчанию (admin_api=open) — прежнее поведение.
    Fixture open(FakeUpstream::Normal, baseConfig(), tokens);
    const Response openDelete = sendRequest(QHostAddress::LocalHost, open.proxy.listenPort(),
                                            request("DELETE", "/api/delete", "secret-token-2"));

    report("34. admin API: allow-list + флаг admin (S-3)",
           clientOk && adminDenied && f.started && phoneTags.status == 200
                   && phoneDelete.status == 403 && laptopDelete.status == 200
                   && openDelete.status == 200 && !tokens.isAdmin(QStringLiteral("phone"))
                   && tokens.isAdmin(QStringLiteral("laptop")),
           QStringLiteral("client=%1 denied=%2 tags=%3 phoneDel=%4 laptopDel=%5 openDel=%6")
                   .arg(clientOk)
                   .arg(adminDenied)
                   .arg(phoneTags.status)
                   .arg(phoneDelete.status)
                   .arg(laptopDelete.status)
                   .arg(openDelete.status));
}

} // namespace


int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    testNoToken();
    testWrongToken();
    testBearerOk();
    testApiKeyOk();
    testStreaming();
    testChunkedBody();
    testHeaderLimit();
    testDeviceNotSpoofable();
    testSpecificAddressWhileLoopbackBusy();
    testBodyLimit();
    testConnectionLimit();
    testNoSocketLeak();
    testBodyLimitActualBytes();
    testBodyLimitLyingContentLength();
    testBackpressureBounded();
    testHeaderTimeout();
    testUpstreamReadTimeout();
    testUpstreamWriteTimeout();
    testUpstreamWriteProgress();
    testDeviceNameValidation();
    testHopByHopHeaders();
    testStrictContentLength();
    testParserTable();
    testUpstreamHostValidation();
    testAdminApiRestriction();

    if (setupTls()) {
        testTlsHandshake();
        testTlsAuth();
        testTlsStreaming();
        testTlsUntrustedClient();
        testTlsCaIdempotent();
        testTlsPendingThreshold();
        testTlsPreHandshakeBudget();
        testTlsSuccessfulHandshakeBudgetLeak();
        testSequentialTlsInProcess();
        testTlsRenewKeepsFiles();
        testTlsSymlinkRefused();
    } else {
        skip("13-28. TLS-сценарии", g_tlsSkipReason);
    }

    std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}
