#ifndef OLLAMAPROXY_H
#define OLLAMAPROXY_H

#include "tokenmap.h"

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

class QTcpServer;
class ProxyConnection;
class ConnectionBudget;

struct ProxyConfig {
    QList<QHostAddress> listenAddresses;                 // для wildcard — {QHostAddress::Any}
    quint16 listenPort = 11434;
    QString upstreamHost = QStringLiteral("127.0.0.1");
    quint16 upstreamPort = 11434;
    int maxHeaderBytes = 32 * 1024;                      // больше → 431
    qint64 maxBodyBytes = 100LL * 1024 * 1024;           // больше → 413
    int idleTimeoutMs = 3600 * 1000;                     // только для релея
    int connectTimeoutMs = 10 * 1000;
    int headerTimeoutMs = 10 * 1000;                     // заголовки не получены → 408
    int upstreamTimeoutMs = 300 * 1000;                  // нет данных от апстрима → 504
    int upstreamWriteTimeoutMs = 60 * 1000;              // апстрим не забирает данные → 504
    int tlsHandshakeTimeoutMs = 10 * 1000;               // TLS handshake не завершён → закрыть
    // 0 — валидное значение «полный отказ»: не обслуживаем ни одного клиента.
    // Иначе — максимум одновременно принятых сокетов, включая TLS-handshake.
    int maxConnections = 256;

    // TLS на плече «клиент (OPNsense) -> прокси». Проверка сертификата — на
    // стороне клиента, поэтому серверный peerVerifyMode = VerifyNone: mTLS
    // не делаем, авторизация всё равно по токену.
    bool tlsEnabled = false;
    QString tlsCertPath;
    QString tlsKeyPath;
};

/**
 * Годится ли строка как адрес апстрима: литерал IP (QHostAddress) либо
 * DNS-имя из [A-Za-z0-9._-]. Нужно, чтобы CRLF/пробелы из config.ini не дали
 * инъекцию в заголовок Host: (L-8).
 */
inline bool isValidUpstreamHost(const QString &host)
{
    if (host.isEmpty() || host.size() > 253)
        return false;
    if (!QHostAddress(host).isNull())
        return true;
    for (const QChar ch : host) {
        const ushort c = ch.unicode();
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok)
            return false;
    }
    return true;
}

/**
 * Встроенный auth-прокси: слушает набор адресов, проверяет API-токены
 * и релеит HTTP/1.1 к локальной Ollama без буферизации.
 *
 * Один QTcpServer на каждый адрес: QTcpServer умеет слушать только один
 * адрес, а по ТЗ нужен список (например, несколько LAN-интерфейсов).
 *
 * Соединения обслуживает внутренний ProxyConnection (см. .cpp) — по одному
 * на пару «клиент ↔ апстрим», с общим таймаутом простоя.
 */
class OllamaProxy : public QObject
{
    Q_OBJECT
public:
    explicit OllamaProxy(QObject *parent = nullptr);
    ~OllamaProxy() override;

    /** Карту можно менять на лету: новые/отозванные токены действуют сразу. */
    void setTokenMap(const TokenMap &map);

    /** Слушает все адреса из cfg; при ошибке любого — закрывает уже открытые. */
    bool start(const ProxyConfig &cfg, QString *error = nullptr);
    void stop();
    bool isRunning() const;

    /** true, если слушатель поднят с TLS. */
    bool isTls() const;

    /** Фактический порт (полезно при listenPort == 0). */
    quint16 listenPort() const;

    /** Открытые адреса в виде "192.168.1.2:11434". */
    QStringList boundEndpoints() const;

    /**
     * Диагностика бюджета соединений: сколько сокетов учтено прямо сейчас
     * (включая те, что ещё в TLS-handshake). После закрытия всех соединений
     * возвращается к 0 — используется тестами, чтобы поймать утечку слота.
     */
    int activeConnections() const;

signals:
    void requestFinished(const QString &device, const QByteArray &method,
                         const QByteArray &path, int status, qint64 bytes);
    void proxyError(const QString &message);

private:
    friend class ProxyConnection;

    void handleNewConnection(QTcpServer *server);
    void discardConnection(ProxyConnection *conn);

    ProxyConfig m_config;
    TokenMap m_tokens;
    QList<QTcpServer *> m_servers;
    QList<ProxyConnection *> m_connections;
    std::shared_ptr<ConnectionBudget> m_budget;
    quint16 m_port = 0;
};

#endif // OLLAMAPROXY_H
