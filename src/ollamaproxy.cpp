#include "ollamaproxy.h"

#include "proxyhead.h"

#include <QDebug>
#include <QFile>
#include <QHash>
#include <QMetaObject>
#include <QSet>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>
#include <utility>

namespace {

/**
 * Backpressure клиент→апстрим и ответ→клиент. Пока буфер одной стороны
 * больше high-water, чтение с другой стороны приостанавливается; возобновляем
 * ниже low-water, чтобы не дёргаться на каждом байте.
 */
const qint64 kBackpressureHighWater = 4LL * 1024 * 1024;
const qint64 kBackpressureLowWater = 1LL * 1024 * 1024;
const qint64 kBackpressureReadChunk = 64LL * 1024;

QByteArray errorResponse(int status, const char *reason, const QByteArray &body)
{
    QByteArray out;
    out += "HTTP/1.1 " + QByteArray::number(status) + ' ' + QByteArray(reason) + "\r\n";
    out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

int parseStatusLine(const QByteArray &line)
{
    // "HTTP/1.1 200 OK"
    const QList<QByteArray> parts = line.simplified().split(' ');
    if (parts.size() < 2)
        return 0;
    bool ok = false;
    const int code = parts.at(1).toInt(&ok);
    return ok ? code : 0;
}

/**
 * Строка для журнала: только печатный ASCII (0x20..0x7e), остальное — '?'.
 * Управляющие байты в path/device/remote позволяют подделать строку journald
 * или нарисовать ANSI-мусор в терминале (L-4).
 */
QByteArray sanitizeForLog(const QByteArray &raw, qsizetype maxBytes = 200)
{
    const qsizetype size = qMin<qsizetype>(raw.size(), maxBytes);
    QByteArray out;
    out.reserve(size + 3);
    for (qsizetype i = 0; i < size; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw.at(i));
        out.append(c >= 0x20 && c <= 0x7e ? static_cast<char>(c) : '?');
    }
    if (raw.size() > maxBytes)
        out += "...";
    return out;
}

/** Устройство из токена: та же политика, что и при выпуске токена (M-3). */
bool deviceNameIsSafe(const QString &device)
{
    return isValidDeviceName(device);
}

/**
 * QSslConfiguration для слушателя. Цепочка и ключ читаются один раз на старте:
 * setSslConfiguration() действует только на последующие соединения, поэтому
 * hot-reload сертификата = stop() + start() (это делает ProxyService).
 */
bool loadTlsConfig(const ProxyConfig &cfg, QSslConfiguration *config, QString *error)
{
    const QList<QSslCertificate> chain = QSslCertificate::fromPath(cfg.tlsCertPath, QSsl::Pem);
    if (chain.isEmpty()) {
        if (error)
            *error = QStringLiteral("не читается сертификат: %1").arg(cfg.tlsCertPath);
        return false;
    }

    QFile keyFile(cfg.tlsKeyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("не читается ключ: %1").arg(cfg.tlsKeyPath);
        return false;
    }
    // EC-ключ (P-256) бэкенд OpenSSL декодирует независимо от algorithm,
    // но на всякий случай пробуем оба варианта.
    QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (key.isNull()) {
        keyFile.seek(0);
        key = QSslKey(&keyFile, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
    }
    if (key.isNull()) {
        if (error)
            *error = QStringLiteral("не читается приватный ключ: %1").arg(cfg.tlsKeyPath);
        return false;
    }

    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setLocalCertificateChain(chain);
    ssl.setLocalCertificate(chain.first());
    ssl.setPrivateKey(key);
    // Клиент — OPNsense, он и проверяет наш сертификат по импортированному CA.
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    *config = std::move(ssl);
    return true;
}

} // namespace

/**
 * Бюджет принятых сокетов на весь прокси. Считает соединения ещё до
 * TLS-handshake — иначе 30 «молчащих» TCP обходят maxConnections (H-2).
 *
 * Живёт в shared_ptr: лямбды сокетов держат его копию, поэтому счётчик
 * корректно освобождается даже если прокси уже перезапущен.
 */
class ConnectionBudget
{
public:
    explicit ConnectionBudget(int limit) : m_limit(limit) {}

    bool tryAcquire()
    {
        if (m_live >= m_limit)
            return false;
        ++m_live;
        return true;
    }

    /** Возврат слота: только через BudgetSlot, поэтому ровно один раз. */
    void release()
    {
        if (m_live > 0)
            --m_live;
    }

    int live() const { return m_live; }

private:
    int m_limit;
    int m_live = 0;
};

namespace {

/**
 * Слот бюджета, привязанный к одному принятому сокету. release() идемпотентен,
 * деструктор — страховка: слот вернётся, даже если destroyed() до сокета не
 * дошёл (например, слушатель уничтожен раньше сокета).
 */
class BudgetSlot
{
public:
    explicit BudgetSlot(std::shared_ptr<ConnectionBudget> budget)
        : m_budget(std::move(budget))
    {
    }
    ~BudgetSlot() { release(); }

    void release()
    {
        if (m_released || !m_budget)
            return;
        m_released = true;
        m_budget->release();
    }

private:
    std::shared_ptr<ConnectionBudget> m_budget;
    bool m_released = false;
};

/**
 * Общая часть plain/TLS-слушателей: учёт сокета в бюджете сразу после accept.
 *
 * Связь destroyed → release принадлежит сокету и слушателю; в отличие от
 * прежнего `socket->disconnect(this)` её никто целиком не снимает. Слот
 * возвращается из destroyed() и (идемпотентно) из ~BudgetedServer, поэтому
 * ни успешный handshake, ни abort, ни stop() не оставляют занятым слот.
 */
class BudgetedServer : public QTcpServer
{
    Q_OBJECT
public:
    BudgetedServer(std::shared_ptr<ConnectionBudget> budget, QObject *parent)
        : QTcpServer(parent), m_budget(std::move(budget))
    {
    }

    ~BudgetedServer() override
    {
        // Сокеты в handshake — дети слушателя; destroyed придёт при их
        // удалении, но слоты возвращаем сразу (release идемпотентен).
        const auto held = std::as_const(m_slots);
        for (const auto &slot : held)
            slot->release();
        m_slots.clear();
    }

protected:
    /** nullptr — лимит исчерпан; сокет учитывать нельзя. */
    std::shared_ptr<BudgetSlot> acquireBudget(QTcpSocket *socket)
    {
        if (!m_budget->tryAcquire())
            return {};
        auto slot = std::make_shared<BudgetSlot>(m_budget);
        QObject::connect(socket, &QObject::destroyed, this, [this, socket, slot] {
            slot->release();
            m_slots.remove(socket);
        });
        m_slots.insert(socket, slot);
        return slot;
    }

private:
    std::shared_ptr<ConnectionBudget> m_budget;
    QHash<QTcpSocket *, std::shared_ptr<BudgetSlot>> m_slots;
};

/** Plain-слушатель: бюджет на accept, ответ 503 сверх лимита. */
class PlainListener : public BudgetedServer
{
    Q_OBJECT
public:
    PlainListener(std::shared_ptr<ConnectionBudget> budget, QObject *parent)
        : BudgetedServer(std::move(budget), parent)
    {
    }

    /** Ответ «слишком много соединений»: TLS здесь ещё нет, можно писать 503. */
    std::function<void(QTcpSocket *)> reject;

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        auto *socket = new QTcpSocket(this);
        if (!socket->setSocketDescriptor(socketDescriptor)) {
            delete socket;
            return;
        }
        if (!acquireBudget(socket)) {
            if (reject)
                reject(socket);
            else
                socket->deleteLater();
            return;
        }
        addPendingConnection(socket);
    }
};

/**
 * TLS-слушатель: единственная точка, где сокет учитывается в лимите до
 * handshake. QSslServer для этого не годится: он создаёт QSslSocket сам и
 * прячет его до encrypted(), а «молчащие» сокеты держит в собственной
 * очереди handshake — учесть их снаружи нельзя.
 */
class TlsListener : public BudgetedServer
{
    Q_OBJECT
public:
    TlsListener(const QSslConfiguration &config, int handshakeTimeoutMs,
                std::shared_ptr<ConnectionBudget> budget, QObject *parent)
        : BudgetedServer(std::move(budget), parent)
        , m_config(config)
        , m_handshakeTimeoutMs(handshakeTimeoutMs)
    {
    }

    std::function<void(QSslSocket *, const QList<QSslError> &)> sslErrorHandler;
    std::function<void(QSslSocket *, QAbstractSocket::SocketError)> socketErrorHandler;

protected:
    void incomingConnection(qintptr socketDescriptor) override
    {
        auto *socket = new QSslSocket(this);
        socket->setSslConfiguration(m_config);
        if (!socket->setSocketDescriptor(socketDescriptor)) {
            delete socket;
            return;
        }
        if (!acquireBudget(socket)) {
            // До handshake ответить нечем: клиент ждёт TLS, а не HTTP.
            socket->abort();
            socket->deleteLater();
            return;
        }

        auto *timer = new QTimer(socket);
        timer->setSingleShot(true);
        timer->setInterval(m_handshakeTimeoutMs);
        QObject::connect(timer, &QTimer::timeout, this, [this, socket] {
            if (socket->isEncrypted())
                return;
            if (socketErrorHandler)
                socketErrorHandler(socket, QAbstractSocket::SocketTimeoutError);
            socket->abort();
            socket->deleteLater();
        });
        timer->start();

        QObject::connect(socket, &QSslSocket::encrypted, this, [this, socket, timer] {
            timer->stop();
            // Дальше сокетом владеет ProxyConnection. Снимаем только
            // обработчики handshake и НЕ трогаем учёт бюджета: destroyed →
            // release должна пережить передачу (H-2).
            QObject::disconnect(socket, &QSslSocket::encrypted, this, nullptr);
            QObject::disconnect(socket, &QSslSocket::sslErrors, this, nullptr);
            QObject::disconnect(socket, &QSslSocket::errorOccurred, this, nullptr);
            addPendingConnection(socket);
            // QTcpServer::incomingConnection() (который мы заменили) сам шлёт
            // newConnection() после addPendingConnection — повторяем это,
            // иначе сокет лежит в очереди, но прокси о нём не узнает.
            emit newConnection();
        });
        QObject::connect(socket, &QSslSocket::sslErrors, this,
                         [this, socket](const QList<QSslError> &errors) {
                             if (sslErrorHandler)
                                 sslErrorHandler(socket, errors);
                         });
        QObject::connect(socket, &QSslSocket::errorOccurred, this,
                         [this, socket](QAbstractSocket::SocketError error) {
                             if (socket->isEncrypted())
                                 return; // ошибки релея обрабатывает ProxyConnection
                             if (socketErrorHandler)
                                 socketErrorHandler(socket, error);
                             socket->abort();
                             socket->deleteLater();
                         });
        socket->startServerEncryption();
    }

private:
    QSslConfiguration m_config;
    int m_handshakeTimeoutMs;
};

} // namespace

/**
 * Одно клиентское соединение: разбор заголовков, авторизация, релей.
 *
 * Класс без Q_OBJECT (сигналы не нужны) — все связи через лямбды. Живёт
 * как child-объект OllamaProxy-списка и удаляется через deleteLater().
 */
class ProxyConnection : public QObject
{
    Q_OBJECT
public:
    ProxyConnection(OllamaProxy *owner, QTcpSocket *client);
    ~ProxyConnection() override;

    void start();
    void abort();

private:
    enum State : quint8 { ReadingHeaders, Connecting, Relaying, Closing, Closed };

    void begin();
    void createUpstream();
    void onClientData();
    void onClientError();
    void onClientClosed();
    void onUpstreamConnected();
    void onUpstreamData();
    void onUpstreamError();
    void onUpstreamClosed();

    void handleHead(const QByteArray &head);
    void touch();
    void fail(int status, const char *reason, const QByteArray &body);
    void shutdownGracefully();
    void finalize();

    void pauseClientReads();
    void resumeClientReads();
    void pauseUpstreamReads();
    void resumeUpstreamReads();
    void stopTimers();

    OllamaProxy *m_owner = nullptr;
    QTcpSocket *m_client = nullptr;
    QTcpSocket *m_upstream = nullptr;
    QTimer *m_idle = nullptr;
    QTimer *m_connectTimer = nullptr;
    QTimer *m_headerTimer = nullptr;
    QTimer *m_upstreamTimer = nullptr;
    QTimer *m_writeTimer = nullptr;

    QMetaObject::Connection m_clientReadConn;
    QMetaObject::Connection m_upstreamReadConn;
    bool m_clientReadPaused = false;
    bool m_upstreamReadPaused = false;

    State m_state = ReadingHeaders;
    bool m_finalized = false;
    bool m_waitingTls = false;  // ждём завершения handshake, данные не трогаем

    QByteArray m_buffer;        // заголовки, затем тело до подключения к апстриму
    QByteArray m_headBlock;     // переписанные заголовки для апстрима
    QByteArray m_probe;         // первые байты ответа — чтобы вытащить статус
    QByteArray m_method;
    QByteArray m_path;
    QByteArray m_version = "HTTP/1.1";
    QString m_device;
    QString m_remote;
    int m_status = 0;
    qint64 m_reqBytes = 0;      // фактически принятые байты тела (H-1)
    qint64 m_respBytes = 0;
};

ProxyConnection::ProxyConnection(OllamaProxy *owner, QTcpSocket *client)
    : m_owner(owner), m_client(client)
{
    // Слушатель создаёт сокет своим ребёнком; после передачи в ProxyConnection
    // он его уже не трогает, поэтому забираем владение к себе — иначе сокет
    // переживёт ProxyConnection и повиснет до удаления сервера.
    m_client->setParent(this);
    m_remote = m_client->peerAddress().toString() + ':'
               + QString::number(m_client->peerPort());

    m_idle = new QTimer(this);
    m_idle->setSingleShot(true);
    m_idle->setInterval(owner->m_config.idleTimeoutMs);
    connect(m_idle, &QTimer::timeout, this, [this] {
        if (m_state == Relaying)
            fail(504, "Gateway Timeout", QByteArrayLiteral("{\"error\":\"idle timeout\"}"));
    });

    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    m_connectTimer->setInterval(owner->m_config.connectTimeoutMs);
    connect(m_connectTimer, &QTimer::timeout, this, [this] {
        if (m_state == Connecting)
            fail(502, "Bad Gateway", QByteArrayLiteral("{\"error\":\"upstream connect timeout\"}"));
    });

    // Абсолютный дедлайн на фазу заголовков: не перезапускается на каждый байт.
    m_headerTimer = new QTimer(this);
    m_headerTimer->setSingleShot(true);
    m_headerTimer->setInterval(owner->m_config.headerTimeoutMs);
    connect(m_headerTimer, &QTimer::timeout, this, [this] {
        if (m_state == ReadingHeaders)
            fail(408, "Request Timeout", QByteArrayLiteral("{\"error\":\"headers timeout\"}"));
    });

    // Нет данных от апстрима: Ollama приняла соединение и «зависла».
    m_upstreamTimer = new QTimer(this);
    m_upstreamTimer->setSingleShot(true);
    m_upstreamTimer->setInterval(owner->m_config.upstreamTimeoutMs);
    connect(m_upstreamTimer, &QTimer::timeout, this, [this] {
        if (m_state == Relaying)
            fail(504, "Gateway Timeout", QByteArrayLiteral("{\"error\":\"upstream timeout\"}"));
    });

    // Апстрим не забирает данные (буфер записи не пустеет) — не держим
    // соединение вечно, пока клиент ждёт.
    m_writeTimer = new QTimer(this);
    m_writeTimer->setSingleShot(true);
    m_writeTimer->setInterval(owner->m_config.upstreamWriteTimeoutMs);
    connect(m_writeTimer, &QTimer::timeout, this, [this] {
        if (m_state == Relaying && m_upstream
            && m_upstream->bytesToWrite() > kBackpressureLowWater) {
            fail(504, "Gateway Timeout",
                 QByteArrayLiteral("{\"error\":\"upstream write timeout\"}"));
        }
    });
}

ProxyConnection::~ProxyConnection() = default;

/** Апстрим создаём лениво: без токена соединение к Ollama не нужно вовсе. */
void ProxyConnection::createUpstream()
{
    m_upstream = new QTcpSocket(this);
    connect(m_upstream, &QTcpSocket::connected, this, [this] { onUpstreamConnected(); });
    m_upstreamReadConn = connect(m_upstream, &QTcpSocket::readyRead, this,
                                 [this] { onUpstreamData(); });
    connect(m_upstream, &QTcpSocket::disconnected, this, [this] { onUpstreamClosed(); });
    connect(m_upstream, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { onUpstreamError(); });
    // Апстрим забрал данные — можно снова читать клиента. Заодно это прогресс
    // записи: write-таймаут отсчитывается от последнего сдвига, а не от
    // момента, когда буфер впервые перевалил за high-water (M-6).
    connect(m_upstream, &QTcpSocket::bytesWritten, this, [this](qint64) {
        if (m_writeTimer->isActive() && m_state == Relaying) {
            if (m_upstream->bytesToWrite() > kBackpressureLowWater)
                m_writeTimer->start();
            else
                m_writeTimer->stop();
        }
        if (!m_clientReadPaused)
            return;
        if (m_upstream->bytesToWrite() <= kBackpressureLowWater) {
            m_writeTimer->stop();
            resumeClientReads();
        }
    });
}

void ProxyConnection::start()
{
    if (m_finalized || m_state == Closed)
        return; // успели оборвать до отложенного старта
    // TlsListener отдаёт уже зашифрованный сокет, но nextPendingConnection()
    // теоретически может вернуть QSslSocket до конца handshake. На этот случай
    // до encrypted() ничего не читаем.
    auto *ssl = qobject_cast<QSslSocket *>(m_client);
    if (ssl && !ssl->isEncrypted()) {
        m_waitingTls = true;
        connect(ssl, &QSslSocket::encrypted, this, [this] {
            if (m_finalized || !m_waitingTls)
                return;
            m_waitingTls = false;
            begin();
        });
        connect(ssl, &QTcpSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError) { onClientError(); });
        connect(ssl, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
            QStringList details;
            details.reserve(errors.size());
            for (const QSslError &error : errors)
                details.append(error.errorString());
            // Токенов здесь нет и быть не может: handshake идёт до заголовков.
            qWarning().noquote() << QStringLiteral("proxy %1 TLS handshake failed: %2")
                                        .arg(QString::fromLatin1(sanitizeForLog(m_remote.toUtf8(), 64)),
                                             details.join(QStringLiteral("; ")));
            abort();
        });
        return;
    }
    begin();
}

void ProxyConnection::stopTimers()
{
    m_idle->stop();
    m_connectTimer->stop();
    m_headerTimer->stop();
    m_upstreamTimer->stop();
    m_writeTimer->stop();
}

void ProxyConnection::begin()
{
    // Сигналы клиента подключаем только здесь: на момент accept/handshake
    // сокет ещё принадлежит слушателю, и вызовы read/write из его стека
    // ломают внутреннее состояние socket engine.
    m_clientReadConn = connect(m_client, &QTcpSocket::readyRead, this,
                               [this] { onClientData(); });
    connect(m_client, &QTcpSocket::disconnected, this, [this] { onClientClosed(); });
    connect(m_client, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) { onClientError(); });
    // Клиент забрал ответ — можно снова читать апстрим.
    connect(m_client, &QTcpSocket::bytesWritten, this, [this](qint64) {
        if (!m_upstreamReadPaused)
            return;
        if (m_client->bytesToWrite() <= kBackpressureLowWater)
            resumeUpstreamReads();
    });

    m_state = ReadingHeaders;
    m_headerTimer->start();
    // Заголовки могли прийти до того, как мы подписались на readyRead.
    if (m_client->bytesAvailable() > 0)
        onClientData();
}

void ProxyConnection::abort()
{
    if (m_state == Closed)
        return;
    m_state = Closed;
    stopTimers();
    if (m_upstream && m_upstream->state() != QAbstractSocket::UnconnectedState)
        m_upstream->abort();
    if (m_client->state() != QAbstractSocket::UnconnectedState)
        m_client->abort();
    finalize();
}

void ProxyConnection::touch()
{
    // Простой отслеживаем только в релее: фаза заголовков ограничена своим
    // абсолютным дедлайном, и продлевать его на каждый байт нельзя (H-3).
    if (m_state == Relaying && m_idle->isActive())
        m_idle->start();
}

void ProxyConnection::pauseClientReads()
{
    if (m_clientReadPaused)
        return;
    m_clientReadPaused = true;
    // Qt перестаёт читать из сокета, когда внутренний буфер полон.
    m_client->setReadBufferSize(kBackpressureReadChunk);
    QObject::disconnect(m_clientReadConn);
    m_clientReadConn = {};
}

void ProxyConnection::resumeClientReads()
{
    if (!m_clientReadPaused || m_state == Closed || m_state == Closing || m_finalized)
        return;
    m_clientReadPaused = false;
    m_client->setReadBufferSize(0); // 0 — без ограничения
    m_clientReadConn = connect(m_client, &QTcpSocket::readyRead, this,
                               [this] { onClientData(); });
    if (m_client->bytesAvailable() > 0) {
        // Не входим в обработчик повторно из стека bytesWritten.
        QMetaObject::invokeMethod(this, [this] { onClientData(); }, Qt::QueuedConnection);
    }
}

void ProxyConnection::pauseUpstreamReads()
{
    if (m_upstreamReadPaused || !m_upstream)
        return;
    m_upstreamReadPaused = true;
    m_upstream->setReadBufferSize(kBackpressureReadChunk);
    QObject::disconnect(m_upstreamReadConn);
    m_upstreamReadConn = {};
}

void ProxyConnection::resumeUpstreamReads()
{
    if (!m_upstreamReadPaused || m_state == Closed || m_state == Closing || m_finalized)
        return;
    m_upstreamReadPaused = false;
    if (!m_upstream)
        return;
    m_upstream->setReadBufferSize(0);
    m_upstreamReadConn = connect(m_upstream, &QTcpSocket::readyRead, this,
                                 [this] { onUpstreamData(); });
    if (m_upstream->bytesAvailable() > 0) {
        QMetaObject::invokeMethod(this, [this] { onUpstreamData(); }, Qt::QueuedConnection);
    }
}

void ProxyConnection::onClientData()
{
    if (m_state == Closed || m_state == Closing || m_finalized || m_waitingTls
        || m_clientReadPaused)
        return;
    touch();

    if (m_state == Relaying) {
        const QByteArray data = m_client->readAll();
        if (data.isEmpty())
            return;
        // Считаем именно переданные байты, а не объявленный Content-Length:
        // chunked и «врущий» маленький CL иначе обходят лимит (H-1).
        m_reqBytes += data.size();
        if (m_reqBytes > m_owner->m_config.maxBodyBytes) {
            fail(413, "Payload Too Large", QByteArrayLiteral("{\"error\":\"body too large\"}"));
            return;
        }
        m_upstream->write(data);
        if (m_upstream->bytesToWrite() > kBackpressureHighWater) {
            pauseClientReads();
            m_writeTimer->start();
        }
        return;
    }

    // ReadingHeaders или Connecting: копим, чтобы ничего не потерять между
    // получением заголовков и установкой соединения с апстримом.
    m_buffer += m_client->readAll();
    if (m_state == Connecting) {
        // В окне connect() тело тоже должно быть ограничено (H-1).
        if (m_buffer.size() > m_owner->m_config.maxBodyBytes) {
            fail(413, "Payload Too Large", QByteArrayLiteral("{\"error\":\"body too large\"}"));
        }
        return;
    }

    const qsizetype end = m_buffer.indexOf("\r\n\r\n");
    if (end < 0) {
        if (m_buffer.size() > m_owner->m_config.maxHeaderBytes)
            fail(431, "Request Header Fields Too Large",
                 QByteArrayLiteral("{\"error\":\"headers too large\"}"));
        return;
    }
    if (end + 4 > m_owner->m_config.maxHeaderBytes) {
        fail(431, "Request Header Fields Too Large",
             QByteArrayLiteral("{\"error\":\"headers too large\"}"));
        return;
    }

    const QByteArray head = m_buffer.left(end + 4);
    m_buffer.remove(0, end + 4); // остаток — начало тела запроса
    handleHead(head);
}

void ProxyConnection::handleHead(const QByteArray &head)
{
    // Заголовки получены и разобраны: абсолютный дедлайн больше не нужен.
    m_headerTimer->stop();

    // Разбор — в чистой функции (src/proxyhead.cpp): та же семантика, но
    // фаззится отдельно от сети (tests/audit/fuzz-head.cpp).
    const ProxyHeadRequest parsed = parseProxyHead(head, m_owner->m_config.maxBodyBytes);

    m_method = parsed.method;
    m_path = parsed.path;
    if (!parsed.version.isEmpty())
        m_version = parsed.version;

    switch (parsed.errorStatus) {
    case 0:
        break;
    case 413:
        fail(413, "Payload Too Large", QByteArrayLiteral("{\"error\":\"body too large\"}"));
        return;
    case 414:
        fail(414, "URI Too Long", QByteArrayLiteral("{\"error\":\"uri too long\"}"));
        return;
    case 431:
        fail(431, "Request Header Fields Too Large",
             QByteArrayLiteral("{\"error\":\"headers too large\"}"));
        return;
    case 501:
        fail(501, "Not Implemented", QByteArrayLiteral("{\"error\":\"method not implemented\"}"));
        return;
    default:
        fail(400, "Bad Request", QByteArrayLiteral("{\"error\":\"bad request\"}"));
        return;
    }

    m_device = m_owner->m_tokens.deviceForToken(parsed.token);
    // Defence in depth: TokenMap уже фильтрует имена, но заголовок не должен
    // собраться из чего-то, кроме [A-Za-z0-9._-] (M-3).
    if (m_device.isEmpty() || !deviceNameIsSafe(m_device)) {
        fail(401, "Unauthorized", QByteArrayLiteral("{\"error\":\"unauthorized\"}"));
        return;
    }

    // S-3: удалить/создать/скачать модели может только токен с флагом admin.
    // Отказ попадает в журнал через requestFinished (статус 403 + device).
    if (m_owner->m_config.restrictAdminApi && !m_owner->m_tokens.isAdmin(m_device)
        && !isClientApiRequest(m_method, m_path)) {
        fail(403, "Forbidden",
             QByteArrayLiteral("{\"error\":\"this token may not use model management "
                               "endpoints\"}"));
        return;
    }

    // Переписываем заголовки: Host — на апстрим, device — из токена,
    // Connection: close, секреты и hop-by-hop заголовки не пересылаем.
    // Hop-by-hop = фиксированный список + всё, что клиент перечислил в
    // Connection: (RFC 9110 §7.6.1). Transfer-Encoding не вырезаем даже если
    // он там перечислен: без него сломается chunked-тело (M-2).
    QSet<QByteArray> hopByHop;
    hopByHop.reserve(parsed.connectionOptions.size() + 12);
    for (const QByteArray &option : parsed.connectionOptions)
        hopByHop.insert(option);
    static const char *const kFixedHopByHop[] = {
        "host",       "x-ollama-device", "connection",     "proxy-connection",
        "keep-alive", "authorization",   "x-api-key",      "te",
        "trailer",    "upgrade",         "proxy-authorization", "proxy-authenticate",
    };
    for (const char *name : kFixedHopByHop)
        hopByHop.insert(QByteArray(name));

    QByteArray out;
    out += m_method + ' ' + m_path + ' ' + m_version + "\r\n";
    for (const auto &header : parsed.headers) {
        const QByteArray &name = header.first;
        const QByteArray lower = name.toLower();
        if (lower == "transfer-encoding") {
            // TE не вырезаем: без него апстрим не разберёт chunked-тело.
            // Парсер уже проверил, что последний coding — chunked.
            out += name + ": " + header.second + "\r\n";
            continue;
        }
        if (hopByHop.contains(lower))
            continue;
        out += name + ": " + header.second + "\r\n";
    }
    out += "Host: " + m_owner->m_config.upstreamHost.toUtf8() + ':'
           + QByteArray::number(m_owner->m_config.upstreamPort) + "\r\n";
    out += "X-Ollama-Device: " + m_device.toUtf8() + "\r\n";
    out += "Connection: close\r\n\r\n";
    m_headBlock = std::move(out);

    if (!m_upstream)
        createUpstream();
    m_state = Connecting;
    m_connectTimer->start();
    m_upstream->connectToHost(m_owner->m_config.upstreamHost, m_owner->m_config.upstreamPort);
}

void ProxyConnection::onUpstreamConnected()
{
    m_connectTimer->stop();
    if (m_state != Connecting)
        return;

    m_state = Relaying;
    // Тело, пришедшее до подключения, тоже идёт в счётчик (H-1).
    m_reqBytes += m_buffer.size();
    if (m_reqBytes > m_owner->m_config.maxBodyBytes) {
        fail(413, "Payload Too Large", QByteArrayLiteral("{\"error\":\"body too large\"}"));
        return;
    }

    QByteArray payload = m_headBlock;
    payload += m_buffer; // тело, пришедшее до подключения
    m_buffer.clear();
    m_upstream->write(payload);

    m_idle->start();
    m_upstreamTimer->start();
    if (m_upstream->bytesToWrite() > kBackpressureHighWater) {
        pauseClientReads();
        m_writeTimer->start();
    }
}

void ProxyConnection::onUpstreamData()
{
    if (m_state == Closed || m_finalized || m_upstreamReadPaused)
        return;
    const QByteArray data = m_upstream->readAll();
    if (data.isEmpty())
        return;
    touch();
    m_upstreamTimer->start(); // любые данные апстрима — прогресс

    if (m_status == 0) {
        m_probe += data;
        if (m_probe.size() > 512)
            m_probe.truncate(512);
        const qsizetype eol = m_probe.indexOf("\r\n");
        if (eol > 0)
            m_status = parseStatusLine(m_probe.left(eol));
    }

    m_respBytes += data.size();
    if (m_state == Relaying) {
        m_client->write(data); // без буферизации: стриминг должен идти сразу
        // Клиент не успевает забирать ответ — притормозим апстрим.
        if (m_client->bytesToWrite() > kBackpressureHighWater)
            pauseUpstreamReads();
    }
}

void ProxyConnection::onUpstreamClosed()
{
    if (m_state == Closed || m_finalized)
        return;
    if (m_state == Connecting) {
        // Апстрим закрылся, не ответив (или порт не тот).
        fail(502, "Bad Gateway", QByteArrayLiteral("{\"error\":\"upstream closed\"}"));
        return;
    }
    shutdownGracefully();
}

void ProxyConnection::onUpstreamError()
{
    if (m_state == Closed || m_state == Closing || m_finalized)
        return;
    if (m_state == Connecting || m_state == ReadingHeaders) {
        fail(502, "Bad Gateway", QByteArrayLiteral("{\"error\":\"upstream unavailable\"}"));
        return;
    }
    // RemoteHostClosedError приходит раньше disconnected. В релее это штатный
    // конец ответа (Connection: close), поэтому не рубим соединение через
    // abort() — иначе потеряем уже записанные клиенту байты.
    shutdownGracefully();
}

void ProxyConnection::onClientClosed()
{
    if (m_finalized)
        return;
    if (m_state == Closing) {
        finalize();
        return;
    }
    abort();
}

void ProxyConnection::onClientError()
{
    if (m_state == Closed || m_state == Closing || m_finalized)
        return;
    if (m_waitingTls) {
        qWarning().noquote() << QStringLiteral("proxy %1 TLS handshake aborted: %2")
                                    .arg(QString::fromLatin1(sanitizeForLog(m_remote.toUtf8(), 64)),
                                         m_client->errorString());
    }
    abort();
}

void ProxyConnection::shutdownGracefully()
{
    if (m_state == Closed || m_state == Closing)
        return;
    m_state = Closing;
    stopTimers();

    if (m_upstream) {
        m_upstream->disconnect(this);
        if (m_upstream->state() != QAbstractSocket::UnconnectedState)
            m_upstream->abort();
    }

    if (m_client->state() == QAbstractSocket::UnconnectedState) {
        finalize();
        return;
    }
    // disconnectFromHost() сначала отправит буфер, потом закроет;
    // финализацию сделает onClientClosed().
    m_client->disconnectFromHost();
    if (m_client->state() == QAbstractSocket::UnconnectedState)
        finalize();
}

void ProxyConnection::fail(int status, const char *reason, const QByteArray &body)
{
    if (m_state == Closed || m_state == Closing || m_finalized)
        return;
    m_status = status;
    m_state = Closing;
    stopTimers();

    if (m_upstream) {
        m_upstream->disconnect(this);
        if (m_upstream->state() != QAbstractSocket::UnconnectedState)
            m_upstream->abort();
    }

    m_client->write(errorResponse(status, reason, body));
    if (m_client->state() == QAbstractSocket::UnconnectedState) {
        finalize();
        return;
    }
    m_client->disconnectFromHost();
    if (m_client->state() == QAbstractSocket::UnconnectedState)
        finalize();
}

void ProxyConnection::finalize()
{
    if (m_finalized)
        return;
    m_finalized = true;
    stopTimers();

    const QByteArray method = m_method.isEmpty() ? QByteArrayLiteral("-") : m_method;
    const QByteArray path = m_path.isEmpty() ? QByteArrayLiteral("-") : sanitizeForLog(m_path);

    // Токены (и их значения) в лог не попадают — только имя устройства.
    qInfo().noquote() << QStringLiteral("proxy %1 device=%2 %3 %4 -> %5, %6 bytes")
                             .arg(QString::fromLatin1(sanitizeForLog(m_remote.toUtf8(), 64)),
                                  m_device.isEmpty()
                                      ? QStringLiteral("-")
                                      : QString::fromLatin1(
                                            sanitizeForLog(m_device.toUtf8(), 64)),
                                  QString::fromLatin1(sanitizeForLog(method, 32)),
                                  QString::fromLatin1(path),
                                  QString::number(m_status), QString::number(m_respBytes));

    // Наружу отдаём уже санитизированный путь: его печатает и GUI, и journald
    // через ProxyService (L-4).
    emit m_owner->requestFinished(m_device, method, path, m_status, m_respBytes);
    m_owner->discardConnection(this);
}

OllamaProxy::OllamaProxy(QObject *parent)
    : QObject(parent)
{
}

OllamaProxy::~OllamaProxy()
{
    stop();
}

void OllamaProxy::setTokenMap(const TokenMap &map)
{
    m_tokens = map;
}

bool OllamaProxy::start(const ProxyConfig &cfg, QString *error)
{
    stop();
    m_budget.reset();
    m_config = cfg;

    const auto failStart = [this, error](const QString &message) {
        if (error)
            *error = message;
        emit proxyError(message);
        return false;
    };

    if (cfg.listenAddresses.isEmpty())
        return failStart(QStringLiteral("не задан ни один адрес прослушивания"));
    // CRLF/пробелы в upstreamHost дали бы инъекцию в "Host:" (L-8).
    if (!isValidUpstreamHost(cfg.upstreamHost))
        return failStart(QStringLiteral("некорректный адрес апстрима: %1").arg(cfg.upstreamHost));

    QSslConfiguration sslConfig;
    if (cfg.tlsEnabled) {
        QString tlsError;
        if (!loadTlsConfig(cfg, &sslConfig, &tlsError))
            return failStart(tlsError);
    }

    // Бюджет общий на все адреса: maxConnections — про процесс, не про listener.
    auto budget = std::make_shared<ConnectionBudget>(cfg.maxConnections);
    m_budget = budget;
    // Пока сокет в handshake, он ещё не в pending-очереди QTcpServer, но
    // бюджет уже считает его. Поэтому отдельный лимит pending-очереди не
    // нужен: бюджет — единственный механизм учёта (H-2).

    quint16 port = cfg.listenPort;
    for (const QHostAddress &address : cfg.listenAddresses) {
        QTcpServer *server = nullptr;

        if (cfg.tlsEnabled) {
            auto *listener = new TlsListener(sslConfig, cfg.tlsHandshakeTimeoutMs, budget, this);
            listener->sslErrorHandler = [](QSslSocket *socket, const QList<QSslError> &errors) {
                QStringList details;
                details.reserve(errors.size());
                for (const QSslError &e : errors)
                    details.append(e.errorString());
                const QString peer = socket ? socket->peerAddress().toString()
                                            : QStringLiteral("-");
                qWarning().noquote()
                    << QStringLiteral("proxy %1 TLS handshake failed: %2")
                           .arg(QString::fromLatin1(sanitizeForLog(peer.toUtf8(), 64)),
                                details.join(QStringLiteral("; ")));
            };
            listener->socketErrorHandler = [](QSslSocket *socket, QAbstractSocket::SocketError) {
                const QString peer = socket ? socket->peerAddress().toString()
                                            : QStringLiteral("-");
                qWarning().noquote() << QStringLiteral("proxy %1 TLS error before encryption")
                                            .arg(QString::fromLatin1(
                                                sanitizeForLog(peer.toUtf8(), 64)));
            };
            server = listener;
            // Сокет попадает в очередь изнутри стека handshake: любые наши
            // действия с ним (даже создание дочерних объектов) — следующим тиком.
            connect(server, &QTcpServer::newConnection, this, [this, server] {
                QMetaObject::invokeMethod(this, [this, server] { handleNewConnection(server); },
                                          Qt::QueuedConnection);
            });
        } else {
            auto *listener = new PlainListener(budget, this);
            listener->reject = [](QTcpSocket *client) {
                client->write(errorResponse(503, "Service Unavailable",
                                            QByteArrayLiteral("{\"error\":\"too many connections\"}")));
                QObject::connect(client, &QTcpSocket::disconnected, client,
                                 &QObject::deleteLater);
                client->disconnectFromHost();
                if (client->state() == QAbstractSocket::UnconnectedState)
                    client->deleteLater();
            };
            server = listener;
            connect(server, &QTcpServer::newConnection, this,
                    [this, server] { handleNewConnection(server); });
        }

        if (!server->listen(address, port)) {
            const QString msg = QStringLiteral("не удалось слушать %1:%2: %3")
                                    .arg(address.toString())
                                    .arg(port)
                                    .arg(server->errorString());
            server->deleteLater();
            // Откатываем уже открытые: частично работающий прокси опаснее
            // полностью выключенного.
            for (QTcpServer *open : std::as_const(m_servers)) {
                open->close();
                open->deleteLater();
            }
            m_servers.clear();
            m_port = 0;
            return failStart(msg);
        }
        m_servers.append(server);

        if (m_port == 0)
            m_port = server->serverPort();
        port = m_port; // при listenPort == 0 все адреса слушают один порт
    }
    return true;
}

void OllamaProxy::stop()
{
    for (QTcpServer *server : std::as_const(m_servers)) {
        server->close();
        server->deleteLater();
    }
    m_servers.clear();

    const QList<ProxyConnection *> connections = m_connections;
    m_connections.clear();
    for (ProxyConnection *conn : connections)
        conn->abort(); // abort() → finalize() → discardConnection() → deleteLater()

    m_port = 0;
}

bool OllamaProxy::isRunning() const
{
    return !m_servers.isEmpty() && m_servers.first()->isListening();
}

bool OllamaProxy::isTls() const
{
    return m_config.tlsEnabled && isRunning();
}

quint16 OllamaProxy::listenPort() const
{
    return m_port;
}

QStringList OllamaProxy::boundEndpoints() const
{
    QStringList list;
    list.reserve(m_servers.size());
    for (const QTcpServer *server : m_servers) {
        const QHostAddress address = server->serverAddress();
        QString host = address.toString();
        if (address.protocol() == QAbstractSocket::IPv6Protocol)
            host = '[' + host + ']';
        list.append(host + ':' + QString::number(server->serverPort()));
    }
    return list;
}

int OllamaProxy::activeConnections() const
{
    return m_budget ? m_budget->live() : 0;
}

void OllamaProxy::handleNewConnection(QTcpServer *server)
{
    while (server->hasPendingConnections()) {
        QTcpSocket *client = server->nextPendingConnection();
        if (!client)
            break;

        // Для TLS бюджет уже проверил лимит на accept; здесь остаётся защита
        // для plain-пути и для гонки «сокет готов, но соединение ещё не создано».
        if (m_connections.size() >= m_config.maxConnections) {
            const auto respond = [client] {
                client->write(errorResponse(503, "Service Unavailable",
                                            QByteArrayLiteral("{\"error\":\"too many connections\"}")));
                client->disconnectFromHost();
            };
            auto *ssl = qobject_cast<QSslSocket *>(client);
            if (ssl && !ssl->isEncrypted()) {
                // 503 нельзя писать в открытый TCP: клиент ждёт TLS.
                connect(ssl, &QSslSocket::encrypted, ssl, respond);
            } else {
                respond();
            }
            connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
            qInfo().noquote() << QStringLiteral("proxy %1 rejected: connection limit %2")
                                     .arg(QString::fromLatin1(
                                              sanitizeForLog(client->peerAddress().toString().toUtf8(), 64)))
                                     .arg(m_config.maxConnections);
            continue;
        }

        auto *conn = new ProxyConnection(this, client);
        m_connections.append(conn);
        // Никакого ввода-вывода внутри стека handshake: стартуем следующим тиком.
        QMetaObject::invokeMethod(conn, [conn] { conn->start(); }, Qt::QueuedConnection);
    }
}

void OllamaProxy::discardConnection(ProxyConnection *conn)
{
    m_connections.removeAll(conn);
    conn->deleteLater();
}

// Q_OBJECT classes defined in this file (A-7).
#include "ollamaproxy.moc"
