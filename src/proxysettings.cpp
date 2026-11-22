#include "proxysettings.h"

#include "apppaths.h"
#include "ollamaproxy.h"
#include "tlsmanager.h"

#include <QDir>
#include <QFileInfo>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QUrl>

#include <utility>

namespace {

QString g_configPathOverride;

const char *kGroup = "proxy";
const char *kDefaultUpstreamHost = "127.0.0.1";

QString formatEndpoint(const QString &host, quint16 port)
{
    // IPv6 в квадратных скобках, иначе "::1:11434" неоднозначен.
    if (host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('[')))
        return QStringLiteral("[%1]:%2").arg(host).arg(port);
    return QStringLiteral("%1:%2").arg(host).arg(port);
}

} // namespace

QString ProxySettings::configPath()
{
    if (!g_configPathOverride.isEmpty())
        return g_configPathOverride;
    return apppaths::configFile();
}

void ProxySettings::setConfigPathOverride(const QString &path)
{
    g_configPathOverride = path;
}

bool ProxySettings::parseOllamaHost(const QString &value, QString *host, quint16 *port)
{
    QString text = value.trimmed();
    if (text.isEmpty())
        return false;
    // OLLAMA_HOST accepts "host", "host:port" and "scheme://host:port".
    if (!text.contains(QStringLiteral("://")))
        text.prepend(QStringLiteral("http://"));
    const QUrl url(text);
    QString h = url.host();
    if (!url.isValid() || h.isEmpty())
        return false;
    // The daemon's bind-all address is reached through loopback.
    if (h == QLatin1String("0.0.0.0") || h == QLatin1String("::"))
        h = QString::fromLatin1(kDefaultUpstreamHost);
    if (!isValidUpstreamHost(h))
        return false;
    *host = h;
    *port = quint16(url.port(11434));
    return true;
}

ProxySettings ProxySettings::load()
{
    ProxySettings s;
    const QString path = configPath();

    // Defaults for the upstream follow OLLAMA_HOST, as the ollama CLI does
    // (D-7); an explicit upstreamHost/upstreamPort in the config wins.
    QString envHost;
    quint16 envPort = 0;
    if (parseOllamaHost(qEnvironmentVariable("OLLAMA_HOST"), &envHost, &envPort)) {
        s.upstreamHost = envHost;
        s.upstreamPort = envPort;
    }

    QSettings ini(path, QSettings::IniFormat);
    ini.beginGroup(QLatin1String(kGroup));

    s.enabled = ini.value(QStringLiteral("enabled"), s.enabled).toBool();

    const QString mode = ini.value(QStringLiteral("mode"), QStringLiteral("interfaces"))
                             .toString()
                             .trimmed()
                             .toLower();
    s.mode = (mode == QLatin1String("all") || mode == QLatin1String("allinterfaces"))
                 ? AllInterfaces
                 : Interfaces;

    const QStringList rawAddresses =
        ini.value(QStringLiteral("addresses")).toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    s.addresses.clear();
    for (const QString &raw : rawAddresses) {
        const QString address = raw.trimmed();
        if (!address.isEmpty())
            s.addresses.append(address);
    }

    const uint port = ini.value(QStringLiteral("port"), s.port).toUInt();
    if (port > 0 && port <= 65535)
        s.port = quint16(port);

    const QString upstreamHost = ini.value(QStringLiteral("upstreamHost"), s.upstreamHost)
                                     .toString()
                                     .trimmed();
    s.upstreamHost = upstreamHost.isEmpty() ? QString::fromLatin1(kDefaultUpstreamHost) : upstreamHost;
    // CRLF/пробелы в имени дали бы инъекцию в "Host:" (L-8): молча
    // откатываемся на дефолт, чтобы прокси вообще мог стартовать.
    if (!isValidUpstreamHost(s.upstreamHost))
        s.upstreamHost = QString::fromLatin1(kDefaultUpstreamHost);

    const uint upstreamPort = ini.value(QStringLiteral("upstreamPort"), s.upstreamPort).toUInt();
    if (upstreamPort > 0 && upstreamPort <= 65535)
        s.upstreamPort = quint16(upstreamPort);

    // Ключ tls отсутствует в старых конфигах — это новый конфиг, по умолчанию
    // internal (см. docs/proxy.md). Явное tls=off уважаем.
    s.tls = tlsModeFromName(ini.value(QStringLiteral("tls"),
                                      tlsModeName(s.tls)).toString(),
                            s.tls);
    s.tlsCert = ini.value(QStringLiteral("tls_cert")).toString().trimmed();
    s.tlsKey = ini.value(QStringLiteral("tls_key")).toString().trimmed();

    if (ini.contains(QStringLiteral("public_url"))) {
        s.publicUrl = ini.value(QStringLiteral("public_url")).toString().trimmed();
    } else {
        // Конфиг старше public_url: раньше DNS-имя было зашито в код и уже
        // стоит в SAN выпущенного сертификата. Берём его оттуда, чтобы
        // обновление не перевыпустило сертификат без имени, по которому
        // клиенты ходят к прокси. Сохранится при следующем save().
        const QStringList names = TlsManager::leafDnsNames();
        if (!names.isEmpty())
            s.publicUrl = QStringLiteral("https://") + names.first();
    }

    // Нет ключа — закрыто (решение 2026-11-22): токенам без флага «Админ»
    // админ-API недоступен. Открыть можно только явным admin_api=open.
    s.adminApiImplicit = !ini.contains(QStringLiteral("admin_api"));
    const QString adminApi = ini.value(QStringLiteral("admin_api"), QStringLiteral("restricted"))
                                 .toString()
                                 .trimmed()
                                 .toLower();
    s.restrictAdminApi = adminApi != QLatin1String("open");

    ini.endGroup();

    // Первый запуск: файла ещё нет — предлагаем все LAN-адреса. Иначе прокси
    // не смог бы стартовать «из коробки» с пустым списком интерфейсов.
    if (s.mode == Interfaces && s.addresses.isEmpty())
        s.addresses = defaultAddresses();

    return s;
}

bool ProxySettings::save(QString *error) const
{
    const QString path = configPath();

    const QFileInfo info(path);
    const QDir dir = info.absoluteDir();
    // Свой каталог — 0700 (S-8); каталог из --config не трогаем.
    if (path == apppaths::configFile())
        apppaths::ensurePrivateDir(dir.absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(dir.absolutePath());
        return false;
    }

    QSettings ini(path, QSettings::IniFormat);
    ini.beginGroup(QLatin1String(kGroup));
    ini.setValue(QStringLiteral("enabled"), enabled);
    ini.setValue(QStringLiteral("mode"),
                 QString::fromLatin1(mode == AllInterfaces ? "all" : "interfaces"));
    ini.setValue(QStringLiteral("addresses"), addresses.join(QLatin1Char(',')));
    ini.setValue(QStringLiteral("port"), port);
    ini.setValue(QStringLiteral("upstreamHost"), upstreamHost);
    ini.setValue(QStringLiteral("upstreamPort"), upstreamPort);
    ini.setValue(QStringLiteral("tls"), tlsModeName(tls));
    ini.setValue(QStringLiteral("tls_cert"), tlsCert);
    ini.setValue(QStringLiteral("tls_key"), tlsKey);
    ini.setValue(QStringLiteral("public_url"), publicUrl);
    ini.setValue(QStringLiteral("admin_api"),
                 QString::fromLatin1(restrictAdminApi ? "restricted" : "open"));
    ini.endGroup();
    ini.sync();
    // Топология сети — не для чужих глаз (S-8).
    apppaths::restrictFile(path);

    if (ini.status() != QSettings::NoError) {
        if (error)
            *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    return true;
}

QList<QHostAddress> ProxySettings::listenAddresses(QString *error) const
{
    QList<QHostAddress> result;
    if (mode == AllInterfaces) {
        result.append(QHostAddress(QHostAddress::Any));
        return result;
    }

    for (const QString &text : addresses) {
        const QHostAddress address(text.trimmed());
        if (address.isNull()) {
            if (error)
                *error = QStringLiteral("invalid address '%1'").arg(text);
            return {};
        }
        if (!result.contains(address))
            result.append(address);
    }

    if (result.isEmpty() && error)
        *error = QStringLiteral("no listen addresses selected");
    return result;
}

QString ProxySettings::describe() const
{
    if (mode == AllInterfaces)
        return formatEndpoint(QStringLiteral("0.0.0.0"), port);

    QStringList parts;
    parts.reserve(addresses.size());
    for (const QString &address : addresses)
        parts.append(formatEndpoint(address, port));
    return parts.join(QStringLiteral(", "));
}

QString ProxySettings::endpoint() const
{
    if (!publicUrl.isEmpty())
        return publicUrl;

    QString host;
    if (mode == AllInterfaces) {
        host = defaultAddresses().value(0, QStringLiteral("127.0.0.1"));
    } else {
        host = addresses.value(0).trimmed();
    }
    if (host.isEmpty())
        return QString();
    if (host.contains(QLatin1Char(':')))
        host = QLatin1Char('[') + host + QLatin1Char(']');
    const QString scheme = tls == TlsMode::Off ? QStringLiteral("http") : QStringLiteral("https");
    return scheme + QStringLiteral("://") + host + QLatin1Char(':') + QString::number(port);
}

QString ProxySettings::publicDnsName() const
{
    const QString host = QUrl(publicUrl).host();
    if (host.isEmpty() || !QHostAddress(host).isNull())
        return QString();
    return host;
}

bool ProxySettings::plaintextOnNetwork() const
{
    if (!enabled || tls != TlsMode::Off)
        return false;
    if (mode == AllInterfaces)
        return true;
    for (const QString &text : addresses) {
        const QHostAddress address(text.trimmed());
        if (!address.isNull() && !address.isLoopback())
            return true;
    }
    return false;
}

QString ProxySettings::tlsModeName(TlsMode mode)
{
    switch (mode) {
    case TlsMode::Off:
        return QStringLiteral("off");
    case TlsMode::Files:
        return QStringLiteral("files");
    case TlsMode::LetsEncrypt:
        return QStringLiteral("letsencrypt");
    case TlsMode::Internal:
        break;
    }
    return QStringLiteral("internal");
}

ProxySettings::TlsMode ProxySettings::tlsModeFromName(const QString &name, TlsMode fallback)
{
    const QString value = name.trimmed().toLower();
    if (value == QLatin1String("off"))
        return TlsMode::Off;
    if (value == QLatin1String("internal"))
        return TlsMode::Internal;
    if (value == QLatin1String("files"))
        return TlsMode::Files;
    if (value == QLatin1String("letsencrypt") || value == QLatin1String("le")
        || value == QLatin1String("acme"))
        return TlsMode::LetsEncrypt;
    return fallback;
}

bool ProxySettings::isLocalUpstreamHost(const QString &host)
{
    const QString h = host.trimmed().toLower();
    return h == QLatin1String("127.0.0.1") || h == QLatin1String("localhost")
           || h == QLatin1String("::1") || h == QLatin1String("0.0.0.0")
           || h == QLatin1String("::");
}

bool ProxySettings::isLinkLocal(const QHostAddress &address)
{
    if (address.protocol() != QAbstractSocket::IPv6Protocol)
        return false;
    // fe80::/10 без scope-id (например, %eth0) не биндится: ядру нужен индекс
    // интерфейса. Такие адреса в списке настроек только путают.
    static const QHostAddress linkLocalPrefix(QStringLiteral("fe80::"));
    return address.isInSubnet(linkLocalPrefix, 10);
}

QStringList ProxySettings::defaultAddresses()
{
    QStringList result;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.isNull() || address.isLoopback() || isLinkLocal(address))
                continue;
            const QString text = address.toString();
            if (!result.contains(text))
                result.append(text);
        }
    }
    return result;
}

bool ProxySettings::conflictsWithLocalUpstream() const
{
    return mode == AllInterfaces && port == upstreamPort && isLocalUpstreamHost(upstreamHost);
}

bool ProxySettings::validateBind(QString *error) const
{
    QString conversionError;
    const QList<QHostAddress> list = listenAddresses(&conversionError);
    if (list.isEmpty()) {
        if (error)
            *error = std::move(conversionError);
        return false;
    }

    for (const QHostAddress &address : list) {
        QTcpServer probe; // стек: деструктор закроет сокет, мусора не остаётся
        if (!probe.listen(address, port)) {
            if (error)
                *error = QStringLiteral("%1: %2")
                             .arg(formatEndpoint(address.toString(), port), probe.errorString());
            return false;
        }
        probe.close();
    }
    return true;
}
