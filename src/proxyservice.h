#ifndef PROXYSERVICE_H
#define PROXYSERVICE_H

#include "proxysettings.h"
#include "tlsmanager.h"
#include "tokenmanager.h"

#include <QDateTime>
#include <QObject>
#include <QString>

class OllamaProxy;
class QThread;
class QTimer;

/**
 * Связывает настройки, KWallet и ядро прокси.
 *
 * Держит OllamaProxy и обновляет карту токенов (TokenMap) на лету: по сигналу
 * TokenManager::tokensChanged (выпуск/отзыв в GUI), при старте и раз в 60 с —
 * кошелёк мог быть разблокирован уже после запуска демона.
 *
 * TLS: перед стартом прокси готовит материал через TlsManager. При ошибке
 * прокси не стартует (тихого downgrade на plain TCP нет). Раз в 12 часов
 * проверяется срок листа; если он близок к истечению, сертификат
 * перевыпускается и слушатель перезапускается — QSslServer применяет
 * конфигурацию только к новым соединениям.
 *
 * Потоки (P-2): OllamaProxy живёт в собственном QThread — модальные окна,
 * запрос пароля KWallet и прочие паузы GUI больше не останавливают
 * проксирование. ProxyService остаётся в GUI-потоке: start()/stop() доходят
 * до прокси блокирующим queued-вызовом (прокси никогда не ждёт GUI, так что
 * взаимной блокировки нет), карта токенов — обычным queued-вызовом, сигналы
 * прокси приходят в GUI-поток через очередь. Состояние (запущен, TLS,
 * адреса) кэшируется после start()/stop(), чтобы statusText() не ходил в
 * чужой поток. Генерация ключей openssl идёт в рабочем потоке (S-10).
 */
class ProxyService : public QObject
{
    Q_OBJECT
public:
    explicit ProxyService(TokenManager *tokens, QObject *parent = nullptr);
    ~ProxyService() override;

    /** Валидация bind -> TLS -> запуск -> первичная загрузка токенов. */
    bool start(const ProxySettings &settings, QString *error = nullptr);
    void stop();
    bool restart(const ProxySettings &settings, QString *error = nullptr);

    /** Перечитать токены; при запертом кошельке оставить прежнюю карту. */
    void reloadTokens();

    bool isRunning() const;
    bool isTls() const;
    ProxySettings settings() const { return m_settings; }

    /** Срок действия листа TLS (невалидная дата, если TLS выключен). */
    QDateTime certificateExpiry() const { return m_certExpiry; }
    QString tlsLastError() const { return m_tlsError; }

    /** Перевыпустить лист (Internal) и перезапустить слушатель. */
    bool renewCertificate(QString *error = nullptr);

    /** "192.0.2.10:11434 · TLS · до 2027-02-18 · токенов: 4" */
    QString statusText() const;

signals:
    void statusChanged();
    void settingsChanged(const ProxySettings &settings);
    void logMessage(const QString &message);
    /** Сертификат перевыпущен/изменён (дата — новый срок действия). */
    void tlsChanged(const QDateTime &expiry);

private:
    bool fail(const QString &message, QString *error);
    QStringList sanAddresses() const;
    TlsManager::TlsMaterial prepareTls(QString *error);
    void checkCertificate();

    /** Выполнить fn в потоке прокси и дождаться результата. */
    template <typename F>
    void callProxy(F &&fn);

    TokenManager *m_tokens;
    QThread *m_proxyThread;
    OllamaProxy *m_proxy;  // живёт в m_proxyThread, удаляется по его finished()
    QTimer *m_timer;
    QTimer *m_certTimer;
    TlsManager *m_tls;
    ProxySettings m_settings;
    QString m_lastError;
    QString m_lastTokenLog;
    QString m_tlsError;
    QDateTime m_certExpiry;
    int m_tokenCount = 0;
    int m_adminCount = 0;
    bool m_running = false;
    bool m_proxyTls = false;
    bool m_tlsBusy = false;  // идёт генерация в рабочем потоке
    QStringList m_endpoints;
};

#endif // PROXYSERVICE_H
