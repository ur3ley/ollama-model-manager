#ifndef TLSMANAGER_H
#define TLSMANAGER_H

#include "proxysettings.h"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>

/**
 * TLS-материал для встроенного прокси: мини-CA + листовой сертификат.
 *
 * Генерация — через внешний `openssl` (QProcess), без новых зависимостей:
 * libssl в Qt уже есть, но Qt не умеет выпускать сертификаты. Если openssl
 * нет или он старше 3.0 — возвращаем понятную ошибку, прокси не стартует
 * (тихого падения на plain TCP быть не должно).
 *
 * Файлы лежат в ~/.config/ollama-manager/tls (0700):
 *   ca.crt / ca.key         Root CA, RSA 4096, 3650 дней
 *   server.crt / server.key лист, RSA 2048, 90 дней, SAN из настроек
 *
 * ensure() идемпотентен: пока SAN совпадает и до истечения больше 30 дней
 * (CA — 60), ни один файл не перегенерируется. CA обновляется отдельно и
 * только сам по себе; при его перевыпуске лист подписывается заново.
 *
 * Файлы пишутся в *.tmp и подменяются rename(2): неудачная генерация не
 * ломает работающую пару. Симлинки на ca.key/server.key/server.crt
 * отклоняются. Соответствие ключа и сертификата проверяется и в internal.
 */
class TlsManager : public QObject
{
    Q_OBJECT
public:
    struct TlsMaterial {
        QString certPath;
        QString keyPath;
        QString caPath;
        QDateTime expires;
        bool ok = false;
        QString error;
    };

    explicit TlsManager(QObject *parent = nullptr);

    /** ~/.config/ollama-manager/tls (GenericConfigLocation). */
    static QString directory();
    /** Подмена каталога для тестов (на этот процесс, в файл не пишется). */
    static void setDirectoryOverride(const QString &path);

    static QString caCertPath();
    static QString caKeyPath();
    static QString certPath();
    static QString keyPath();

    /** "openssl 3.0.13 ..." в *version; false — нет бинарника или он < 3.0. */
    static bool opensslAvailable(QString *version = nullptr);

    /**
     * Материал под запрошенный режим.
     *
     * addresses — адреса для SAN (IP:...), dnsName — DNS:..., обычно хост из
     * TokenManager::endpoint(). Для Off — пустой ok-материал, для Files —
     * проверка пары cert/key, для LetsEncrypt — ошибка «позже».
     */
    TlsMaterial ensure(ProxySettings::TlsMode mode, const QStringList &addresses,
                       const QString &dnsName, QString *error = nullptr);

    /** Пути для режима Files: ensure() их проверит и вернёт как материал. */
    void setFilePaths(const QString &cert, const QString &key);

    /** Копия ca.crt в target (для импорта в OPNsense). */
    bool exportCa(const QString &target, QString *error = nullptr);

    /** Срок листа из последнего ensure() (невалидная дата, если TLS выкл.). */
    QDateTime expiry() const { return m_last.expires; }
    bool isValid() const { return m_last.ok; }
    QString lastError() const { return m_last.error; }

    /**
     * Запросить перевыпуск листа: следующий ensure() выпустит новый на том же
     * CA. Старые файлы остаются рабочими до успешной атомарной подмены.
     */
    void invalidateLeaf();

private:
    TlsMaterial m_last;
    bool m_renewLeaf = false;
    QString m_filesCert;
    QString m_filesKey;
};

#endif // TLSMANAGER_H
