#ifndef PROXYSETTINGS_H
#define PROXYSETTINGS_H

#include <QHostAddress>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * Настройки встроенного auth-прокси.
 *
 * Хранятся в отдельном INI-файле (~/.config/ollama-manager/config.ini), а не в
 * дефолтном QSettings(): путь последнего зависит от имени приложения и
 * организации, а они меняются между версиями. Файл один и тот же для GUI и для
 * юнита, поэтому настройки, выставленные в окне, действуют и в демоне.
 */
struct ProxySettings
{
    enum Mode : quint8 { Interfaces, AllInterfaces };

    /**
     * TLS на плече «OPNsense -> прокси».
     *
     * LetsEncrypt пока только хранится в файле: ACME (DNS-01/deSEC) появится
     * позже, до тех пор прокси с таким режимом не стартует и говорит об этом.
     */
    enum class TlsMode : quint8 { Off, Internal, Files, LetsEncrypt };

    bool enabled = true;
    Mode mode = Interfaces;
    QStringList addresses;          // выбранные адреса, напр. {"192.0.2.10"}
    quint16 port = 11434;
    QString upstreamHost = QStringLiteral("127.0.0.1");
    quint16 upstreamPort = 11434;

    TlsMode tls = TlsMode::Internal; // свой мини-CA — рабочий дефолт
    QString tlsCert;                // только для Files
    QString tlsKey;

    /**
     * Адрес, по которому клиенты (OPNsense, устройства) ходят к прокси:
     * [proxy] public_url. Показывается в диалоге токенов, его DNS-имя
     * попадает в SAN внутреннего сертификата. Пусто — адрес вычисляется из
     * адреса прослушивания (S-4: личный домен больше не зашит в код).
     */
    QString publicUrl;

    /**
     * [proxy] admin_api: true ("restricted", по умолчанию с 2026-11-22) —
     * токен без флага admin получает только клиентский API
     * (чат/генерация/эмбеддинги/список), а /api/delete, /api/create,
     * /api/pull и т.п. — 403 (S-3); false ("open") — любой токен может всё.
     * Ключа нет в конфиге (он старше этой настройки) — restricted.
     */
    bool restrictAdminApi = true;
    /** Ключа admin_api в конфиге не было: значение взято по умолчанию. */
    bool adminApiImplicit = false;

    /** OLLAMA_HOST ("host", "host:port", "http://host:port") → upstream host/port. */
    static bool parseOllamaHost(const QString &value, QString *host, quint16 *port);

    static QString tlsModeName(TlsMode mode);     // "off" | "internal" | ...
    static TlsMode tlsModeFromName(const QString &name, TlsMode fallback = TlsMode::Internal);

    /** ~/.config/ollama-manager/config.ini (GenericConfigLocation). */
    static QString configPath();
    /** Подмена пути для --config (на этот запуск, в файл не пишется). */
    static void setConfigPathOverride(const QString &path);

    static ProxySettings load();
    bool save(QString *error = nullptr) const;

    /** AllInterfaces -> {QHostAddress::Any}; Interfaces -> адреса из addresses. */
    QList<QHostAddress> listenAddresses(QString *error = nullptr) const;

    /** "192.0.2.10:11434" / "0.0.0.0:11435". */
    QString describe() const;

    /** publicUrl, а если он пуст — <http|https>://<первый адрес>:<port>. */
    QString endpoint() const;

    /** DNS-имя из publicUrl для SAN; пусто, если publicUrl пуст или это IP. */
    QString publicDnsName() const;

    /**
     * TLS выключен, а слушаем не только loopback: Bearer-токены пойдут по
     * сети открытым текстом (S-5).
     */
    bool plaintextOnNetwork() const;

    /** true, если апстрим локальный и порт совпадает в режиме «Все интерфейсы». */
    bool conflictsWithLocalUpstream() const;

    /**
     * Реальная проверка bind: временный QTcpServer на каждый адрес/порт,
     * сразу закрывается. Временных сокетов не остаётся.
     */
    bool validateBind(QString *error = nullptr) const;

    static bool isLocalUpstreamHost(const QString &host);

    /** IPv6 link-local (fe80::/10) без scope-id не биндится — такие не предлагаем. */
    static bool isLinkLocal(const QHostAddress &address);

    /** Пригодные для прослушивания адреса интерфейсов (без loopback/link-local). */
    static QStringList defaultAddresses();
};

#endif // PROXYSETTINGS_H
