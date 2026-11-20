#include "proxyservice.h"

#include "i18n.h"
#include "ollamaproxy.h"
#include "tokenmap.h"

#include <QHostAddress>
#include <QStringList>
#include <QTimer>
#include <QUrl>

#include <utility>

namespace {

quint16 suggestedPort(quint16 port)
{
    if (port >= 65535)
        return quint16(65534);
    return quint16(port + 1);
}

/** Период фоновой проверки срока листа. */
const int kCertCheckIntervalMs = 12 * 3600 * 1000;

QString formatDate(const QDateTime &date)
{
    if (!date.isValid())
        return QStringLiteral("-");
    return date.toLocalTime().toString(QStringLiteral("yyyy-MM-dd"));
}

} // namespace

ProxyService::ProxyService(TokenManager *tokens, QObject *parent)
    : QObject(parent)
    , m_tokens(tokens)
    , m_proxy(new OllamaProxy(this))
    , m_timer(new QTimer(this))
    , m_certTimer(new QTimer(this))
    , m_tls(new TlsManager(this))
    , m_settings(ProxySettings::load())
{
    m_timer->setInterval(60 * 1000);
    connect(m_timer, &QTimer::timeout, this, &ProxyService::reloadTokens);

    m_certTimer->setInterval(kCertCheckIntervalMs);
    connect(m_certTimer, &QTimer::timeout, this, &ProxyService::checkCertificate);

    if (m_tokens) {
        connect(m_tokens, &TokenManager::tokensChanged, this, &ProxyService::reloadTokens);
    }

    connect(m_proxy, &OllamaProxy::requestFinished, this,
            [this](const QString &device, const QByteArray &method, const QByteArray &path,
                   int status, qint64 bytes) {
                QStringList parts;
                parts << (device.isEmpty() ? QStringLiteral("-") : device)
                      << QString::fromLatin1(method)
                      << QString::fromUtf8(path)
                      << QStringLiteral("-> %1").arg(status)
                      << QStringLiteral("(%1 bytes)").arg(bytes);
                emit logMessage(parts.join(QLatin1Char(' ')));
            });

    connect(m_proxy, &OllamaProxy::proxyError, this, &ProxyService::logMessage);
}

ProxyService::~ProxyService() = default;

bool ProxyService::fail(const QString &message, QString *error)
{
    m_lastError = message;
    if (error)
        *error = message;
    emit logMessage(T("proxy.logStartError").arg(message));
    emit statusChanged();
    return false;
}

QStringList ProxyService::sanAddresses() const
{
    // SAN должен покрывать адреса, по которым к прокси придут. Loopback и
    // link-local в сертификат не кладём — OPNsense ходит по LAN-адресу.
    QStringList source;
    if (m_settings.mode == ProxySettings::AllInterfaces)
        source = ProxySettings::defaultAddresses();
    else
        source = m_settings.addresses;

    QStringList result;
    for (const QString &raw : std::as_const(source)) {
        const QHostAddress address(raw.trimmed());
        if (address.isNull() || address.isLoopback() || ProxySettings::isLinkLocal(address))
            continue;
        const QString text = address.toString();
        if (!result.contains(text))
            result.append(text);
    }

    // Прокси может слушать только loopback (ручная проверка, отладка) — тогда
    // сертификат без IP был бы бесполезен вовсе.
    if (result.isEmpty())
        result.append(QStringLiteral("127.0.0.1"));
    return result;
}

TlsManager::TlsMaterial ProxyService::prepareTls(QString *error)
{
    m_tls->setFilePaths(m_settings.tlsCert, m_settings.tlsKey);

    QString dnsName;
    if (m_settings.tls == ProxySettings::TlsMode::Internal) {
        // OPNsense обращается по имени из endpoint() — оно должно быть в SAN.
        dnsName = QUrl(TokenManager::endpoint()).host();
    }

    TlsManager::TlsMaterial material =
        m_tls->ensure(m_settings.tls, sanAddresses(), dnsName, error);
    if (material.ok) {
        m_tlsError.clear();
        if (material.expires.isValid())
            m_certExpiry = material.expires;
    } else {
        m_tlsError = material.error;
        if (error && error->isEmpty())
            *error = material.error;
    }
    return material;
}

bool ProxyService::start(const ProxySettings &settings, QString *error)
{
    m_lastError.clear();
    m_lastTokenLog.clear();

    if (!settings.enabled) {
        stop();
        m_settings = settings;
        m_tokenCount = 0;
        m_certExpiry = QDateTime();
        m_tlsError.clear();
        emit logMessage(T("proxy.logDisabled"));
        emit settingsChanged(m_settings);
        emit statusChanged();
        return true;
    }

    m_settings = settings;

    // TLS готовим ДО stop(): генерация RSA может занять секунды и упасть,
    // а работающий слушатель при этом должен остаться на месте (M-5).
    // Файлы старых сертификатов не трогаются до успешной генерации новых.
    QString tlsError;
    const TlsManager::TlsMaterial material = prepareTls(&tlsError);
    if (!material.ok) {
        m_certExpiry = QDateTime();
        return fail(T("proxy.errTls").arg(tlsError), error);
    }

    // Дальше слушатель нужно погасить: validateBind проверяет реальный bind,
    // а собственный занятый порт иначе даст ложную ошибку.
    stop();

    // Валидация до старта: реальный bind, а не сравнение чисел.
    QString bindError;
    if (!m_settings.validateBind(&bindError)) {
        // Отдельно объясняем самый частый случай: в режиме «Все интерфейсы»
        // 0.0.0.0:11434 конфликтует с локальной Ollama на 127.0.0.1:11434.
        const QString message = m_settings.conflictsWithLocalUpstream()
                                    ? T("proxy.errPortOllama")
                                          .arg(m_settings.port)
                                          .arg(suggestedPort(m_settings.port))
                                    : T("proxy.errBind").arg(bindError);
        return fail(message, error);
    }

    QString conversionError;
    ProxyConfig config;
    config.listenAddresses = m_settings.listenAddresses(&conversionError);
    if (config.listenAddresses.isEmpty()) {
        return fail(T("proxy.errBind").arg(conversionError), error);
    }
    config.listenPort = m_settings.port;
    config.upstreamHost = m_settings.upstreamHost;
    config.upstreamPort = m_settings.upstreamPort;
    config.tlsEnabled = m_settings.tls != ProxySettings::TlsMode::Off;
    config.tlsCertPath = material.certPath;
    config.tlsKeyPath = material.keyPath;

    QString proxyError;
    if (!m_proxy->start(config, &proxyError)) {
        return fail(T("proxy.errBind").arg(proxyError), error);
    }

    m_tokenCount = 0;
    reloadTokens();
    m_timer->start();
    if (m_settings.tls == ProxySettings::TlsMode::Internal
        || m_settings.tls == ProxySettings::TlsMode::Files) {
        m_certTimer->start();
    }

    emit logMessage(T("proxy.logStarted").arg(m_proxy->boundEndpoints().join(QStringLiteral(", "))));
    if (m_proxy->isTls()) {
        if (m_certExpiry.isValid())
            emit logMessage(T("proxy.logTlsExpiry").arg(formatDate(m_certExpiry)));
        emit tlsChanged(m_certExpiry);
    }
    emit settingsChanged(m_settings);
    emit statusChanged();
    return true;
}

void ProxyService::stop()
{
    const bool wasRunning = isRunning();
    m_timer->stop();
    m_certTimer->stop();
    m_proxy->stop();
    m_tokenCount = 0;

    if (wasRunning)
        emit logMessage(T("proxy.logStopped"));
    emit statusChanged();
}

bool ProxyService::restart(const ProxySettings &settings, QString *error)
{
    // Прежний рабочий конфиг: если новый не поднимется, возвращаемся к нему,
    // а не оставляем прокси выключенным (M-5).
    const ProxySettings working = m_settings;
    const bool wasRunning = isRunning();

    QString failure;
    if (start(settings, &failure))
        return true;

    // Если start() упал до stop() (например, не подготовился TLS), старый
    // слушатель всё ещё работает — откат не нужен и только добавил бы простой.
    if (wasRunning && !isRunning()) {
        QString rollbackError;
        if (start(working, &rollbackError)) {
            emit logMessage(QStringLiteral("apply failed (%1); rolled back to the previous "
                                           "working configuration")
                                .arg(failure));
        } else {
            emit logMessage(QStringLiteral("apply failed (%1); rollback also failed: %2")
                                .arg(failure, rollbackError));
        }
    }
    if (error)
        *error = std::move(failure);
    return false;
}

bool ProxyService::renewCertificate(QString *error)
{
    const auto failRenew = [&](const QString &message) {
        if (error)
            *error = message;
        emit logMessage(T("proxy.logStartError").arg(message));
        return false;
    };

    if (!m_settings.enabled)
        return failRenew(QStringLiteral("proxy is disabled"));
    if (m_settings.tls == ProxySettings::TlsMode::Off)
        return failRenew(QStringLiteral("TLS is disabled"));
    if (m_settings.tls == ProxySettings::TlsMode::Files) {
        return failRenew(
            QStringLiteral("certificate files are managed outside the application"));
    }
    if (m_settings.tls == ProxySettings::TlsMode::LetsEncrypt)
        return failRenew(QStringLiteral("Let's Encrypt mode is not implemented yet"));

    // Лист забываем, CA остаётся: перевыпуск не требует нового импорта в OPNsense.
    m_tls->invalidateLeaf();

    QString restartError;
    if (!restart(m_settings, &restartError)) {
        if (error)
            *error = std::move(restartError);
        return false;
    }
    if (m_certExpiry.isValid())
        emit logMessage(T("proxy.logTlsRenewed").arg(formatDate(m_certExpiry)));
    return true;
}

void ProxyService::checkCertificate()
{
    if (!isRunning())
        return;
    if (m_settings.tls != ProxySettings::TlsMode::Internal)
        return; // у Files срок отслеживает владелец файлов

    const QDateTime before = m_certExpiry;
    QString error;
    const TlsManager::TlsMaterial material = prepareTls(&error);
    if (!material.ok) {
        emit logMessage(T("proxy.errTls").arg(error));
        emit statusChanged();
        return;
    }
    if (material.expires == before)
        return; // ensure() ничего не перегенерировал

    m_certExpiry = material.expires;
    emit tlsChanged(m_certExpiry);
    if (m_certExpiry.isValid())
        emit logMessage(T("proxy.logTlsRenewed").arg(formatDate(m_certExpiry)));

    // QSslServer применит новый сертификат только к новым соединениям, поэтому
    // продление = перезапуск слушателя.
    QString restartError;
    if (!restart(m_settings, &restartError))
        emit logMessage(T("proxy.errTls").arg(restartError));
}

void ProxyService::reloadTokens()
{
    if (!m_tokens)
        return;

    QString error;
    QList<TokenManager::Entry> entries = m_tokens->entriesIfUnlocked(&error);

    if (!error.isEmpty()) {
        // Кошелёк заперт (или недоступен): держим прежнюю карту, как старый
        // скрипт, и пишем в лог одну строку — но не раз в минуту.
        const QString message = T("proxy.logWalletLocked").arg(error);
        if (message != m_lastTokenLog) {
            m_lastTokenLog = message;
            emit logMessage(message);
        }
        return;
    }
    m_lastTokenLog.clear();

    QList<QPair<QByteArray, QString>> tokens;
    tokens.reserve(entries.size());
    for (TokenManager::Entry &entry : entries) {
        if (entry.token.isEmpty() || entry.device.isEmpty())
            continue;
        // toUtf8() — временная копия секрета в куче; затираем и её, и
        // QString-копию из кошелька, чтобы они не дожили до core dump (L-3).
        QByteArray token = entry.token.toUtf8();
        entry.token.fill(QChar(0));
        tokens.append(qMakePair(token, entry.device));
    }

    TokenMap map;
    map.setTokens(tokens);
    for (auto &pair : tokens)
        pair.first.fill('\0');
    tokens.clear();
    QList<TokenManager::Entry>().swap(entries);

    m_proxy->setTokenMap(map);

    if (m_tokenCount != map.size()) {
        m_tokenCount = map.size();
        emit statusChanged();
    }
}

bool ProxyService::isRunning() const
{
    return m_proxy->isRunning();
}

bool ProxyService::isTls() const
{
    return m_proxy->isTls();
}

QString ProxyService::statusText() const
{
    if (isRunning()) {
        if (m_proxy->isTls()) {
            return T("proxy.statusRunningTls")
                .arg(m_proxy->boundEndpoints().join(QStringLiteral(", ")))
                .arg(formatDate(m_certExpiry))
                .arg(m_tokenCount);
        }
        return T("proxy.statusRunning")
            .arg(m_proxy->boundEndpoints().join(QStringLiteral(", ")))
            .arg(m_tokenCount);
    }
    if (!m_settings.enabled)
        return T("proxy.statusDisabled");
    if (!m_lastError.isEmpty())
        return T("proxy.statusError").arg(m_lastError);
    return T("proxy.statusStopped");
}
