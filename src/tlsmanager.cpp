#include "tlsmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QProcess>
#include <QRandomGenerator>
#include <QSet>
#include <QSslCertificate>
#include <QSslKey>
#include <QStandardPaths>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {

QString g_directoryOverride;

const char *kCaCommonName = "Ollama Manager Internal CA";
const int kCaDays = 3650;
const int kLeafDays = 90;
const int kLeafRenewDays = 30;
const int kCaRenewDays = 60;

QString tlsDirectory()
{
    if (!g_directoryOverride.isEmpty())
        return g_directoryOverride;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(base).filePath(QStringLiteral("ollama-manager/tls"));
}

/**
 * Запуск openssl. При ошибке в *error попадает stderr — он у openssl
 * человекочитаемый и не содержит секретов (ключи читаются из файлов).
 */
bool runOpenSsl(const QStringList &args, const QString &workDir, QString *error,
                QByteArray *output = nullptr)
{
    QProcess process;
    process.setProgram(QStringLiteral("openssl"));
    process.setArguments(args);
    if (!workDir.isEmpty())
        process.setWorkingDirectory(workDir);

    process.start();
    if (!process.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("не удалось запустить openssl: %1")
                         .arg(process.errorString());
        return false;
    }
    // RSA 4096 на слабом железе генерируется пару секунд; 3 минуты — с запасом.
    if (!process.waitForFinished(180000)) {
        process.kill();
        process.waitForFinished(2000);
        if (error)
            *error = QStringLiteral("openssl не завершился за 180 с");
        return false;
    }

    const QByteArray stdoutData = process.readAllStandardOutput();
    if (output)
        *output = stdoutData;

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QString text = QString::fromUtf8(process.readAllStandardError()).trimmed();
        if (text.isEmpty())
            text = QStringLiteral("openssl exit code %1").arg(process.exitCode());
        if (error)
            *error = std::move(text);
        return false;
    }
    return true;
}

bool ensureDirectory(const QString &path, QString *error)
{
    // Каталог с ключами тоже не должен быть симлинком: chmod/openssl пошли бы
    // по чужому пути (L-2).
    if (QFileInfo(path).isSymLink()) {
        if (error)
            *error = QStringLiteral("%1 — символическая ссылка, отказываюсь использовать")
                         .arg(path);
        return false;
    }
    QDir dir(path);
    const bool existed = dir.exists();
    if (!existed && !dir.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("не удалось создать каталог %1").arg(path);
        return false;
    }
    // Права меняем только у каталога, который создали сами: чужой каталог
    // (даже свой по uid) не наше дело.
    if (!existed) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                       | QFileDevice::ExeOwner);
    }
    return true;
}

/** Атомарно заменить finalPath содержимым tmpPath (POSIX rename(2)). */
bool replaceFile(const QString &tmpPath, const QString &finalPath, QString *error)
{
    if (QFileInfo(finalPath).isSymLink()) {
        if (error)
            *error = QStringLiteral("%1 — символическая ссылка").arg(finalPath);
        return false;
    }
    const QByteArray from = QFile::encodeName(tmpPath);
    const QByteArray to = QFile::encodeName(finalPath);
    if (std::rename(from.constData(), to.constData()) != 0) {
        if (error)
            *error = QStringLiteral("не удалось заменить %1: %2")
                         .arg(finalPath, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    return true;
}

void makePrivate(const QString &path)
{
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QSslCertificate loadCert(const QString &path)
{
    return QSslCertificate::fromPath(path, QSsl::Pem).value(0);
}

QDateTime certExpiry(const QSslCertificate &cert)
{
    if (cert.isNull())
        return {};
    return cert.expiryDate();
}

bool expiresWithin(const QDateTime &date, int days)
{
    if (!date.isValid())
        return true;
    return date.toUTC() <= QDateTime::currentDateTimeUtc().addDays(days);
}

QString normalizedIp(const QString &text)
{
    const QHostAddress address(text.trimmed());
    if (!address.isNull())
        return address.toString().toLower();
    return text.trimmed().toLower();
}

QSet<QString> desiredIps(const QStringList &addresses)
{
    QSet<QString> result;
    for (const QString &raw : addresses) {
        const QString text = raw.trimmed();
        if (text.isEmpty())
            continue;
        const QHostAddress address(text);
        if (address.isNull())
            continue; // мусор в списке адресов — не повод падать
        result.insert(address.toString().toLower());
    }
    return result;
}

QSet<QString> certIps(const QSslCertificate &cert)
{
    QSet<QString> result;
    const QMultiMap<QSsl::AlternativeNameEntryType, QString> names =
        cert.subjectAlternativeNames();
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        if (it.key() == QSsl::IpAddressEntry)
            result.insert(normalizedIp(it.value()));
    }
    return result;
}

QStringList certDnsNames(const QSslCertificate &cert)
{
    QStringList result;
    const QMultiMap<QSsl::AlternativeNameEntryType, QString> names =
        cert.subjectAlternativeNames();
    for (auto it = names.constBegin(); it != names.constEnd(); ++it) {
        if (it.key() == QSsl::DnsEntry)
            result.append(it.value().toLower());
    }
    result.sort();
    return result;
}

/** SAN для leaf: IP:... на каждый адрес + DNS:... на имя из endpoint. */
QString sanValue(const QSet<QString> &ips, const QString &dnsName)
{
    QStringList parts;
    QStringList ipList = QStringList(ips.constBegin(), ips.constEnd());
    ipList.sort();
    for (const QString &ip : std::as_const(ipList))
        parts.append(QStringLiteral("IP:%1").arg(ip));
    if (!dnsName.trimmed().isEmpty())
        parts.append(QStringLiteral("DNS:%1").arg(dnsName.trimmed()));
    return parts.join(QLatin1Char(','));
}

QByteArray randomSerial()
{
    // 63 бита: старший бит не выставлен, чтобы serial остался положительным.
    const quint64 value = QRandomGenerator::global()->generate64() >> 1u;
    return QByteArrayLiteral("0x") + QByteArray::number(value, 16);
}

/** Одинаковый публичный ключ у cert и key? Сравнение через openssl. */
bool keyMatchesCert(const QString &certFile, const QString &keyFile, QString *error)
{
    QByteArray certPub;
    if (!runOpenSsl({QStringLiteral("x509"), QStringLiteral("-noout"), QStringLiteral("-pubkey"),
                     QStringLiteral("-in"), certFile},
                    {}, error, &certPub))
        return false;

    QByteArray keyPub;
    if (!runOpenSsl({QStringLiteral("pkey"), QStringLiteral("-pubout"), QStringLiteral("-in"),
                     keyFile},
                    {}, error, &keyPub))
        return false;

    // PEM-обёртки могут отличаться (\r\n); важен сам base64-блок.
    const auto strip = [](const QByteArray &pem) {
        QByteArray out;
        for (const QByteArray &line : pem.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.startsWith('-'))
                continue;
            out += trimmed;
        }
        return out;
    };
    if (strip(certPub) != strip(keyPub)) {
        if (error)
            *error = QStringLiteral("приватный ключ не соответствует сертификату");
        return false;
    }
    return true;
}

} // namespace

TlsManager::TlsManager(QObject *parent)
    : QObject(parent)
{
}

QString TlsManager::directory()
{
    return tlsDirectory();
}

void TlsManager::setDirectoryOverride(const QString &path)
{
    g_directoryOverride = path;
}

QString TlsManager::caCertPath()
{
    return QDir(tlsDirectory()).filePath(QStringLiteral("ca.crt"));
}

QString TlsManager::caKeyPath()
{
    return QDir(tlsDirectory()).filePath(QStringLiteral("ca.key"));
}

QString TlsManager::certPath()
{
    return QDir(tlsDirectory()).filePath(QStringLiteral("server.crt"));
}

QString TlsManager::keyPath()
{
    return QDir(tlsDirectory()).filePath(QStringLiteral("server.key"));
}

bool TlsManager::opensslAvailable(QString *version)
{
    QByteArray output;
    QString ignored;
    if (!runOpenSsl({QStringLiteral("version")}, {}, &ignored, &output))
        return false;

    const QString text = QString::fromUtf8(output).trimmed();
    if (version)
        *version = text;

    // "OpenSSL 3.0.13 30 Jan 2024" -> major=3. Нам нужны -addext и -set_serial,
    // они есть уже в 1.1.1, но 3.0 — то, что реально стоит проверять.
    const QStringList parts = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2)
        return false;
    const QString &number = parts.at(1);
    bool ok = false;
    const int major = number.section(QLatin1Char('.'), 0, 0).toInt(&ok);
    return ok && major >= 3;
}

TlsManager::TlsMaterial TlsManager::ensure(ProxySettings::TlsMode mode, const QStringList &addresses,
                                           const QString &dnsName, QString *error)
{
    if (error)
        error->clear();

    TlsMaterial material;
    const auto failWith = [&](const QString &message) {
        material.ok = false;
        material.error = message;
        m_last = material;
        if (error)
            *error = message;
        return material;
    };

    if (mode == ProxySettings::TlsMode::Off) {
        material.ok = true;
        m_last = material;
        return material;
    }

    if (mode == ProxySettings::TlsMode::LetsEncrypt)
        return failWith(QStringLiteral("режим Let's Encrypt будет добавлен позже"));

    QString version;
    if (!opensslAvailable(&version))
        return failWith(QStringLiteral("openssl ≥ 3.0 не найден в PATH — TLS недоступен"));

    if (mode == ProxySettings::TlsMode::Files) {
        const QString certFile = m_filesCert;
        const QString keyFile = m_filesKey;
        if (certFile.isEmpty() || keyFile.isEmpty())
            return failWith(QStringLiteral("не заданы файл сертификата и ключа"));
        if (!QFileInfo::exists(certFile))
            return failWith(QStringLiteral("нет файла сертификата: %1").arg(certFile));
        if (!QFileInfo::exists(keyFile))
            return failWith(QStringLiteral("нет файла ключа: %1").arg(keyFile));

        const QSslCertificate cert = loadCert(certFile);
        if (cert.isNull())
            return failWith(QStringLiteral("не читается сертификат: %1").arg(certFile));

        QFile keyHandle(keyFile);
        if (!keyHandle.open(QIODevice::ReadOnly))
            return failWith(QStringLiteral("не читается ключ: %1").arg(keyFile));
        QSslKey sslKey(&keyHandle, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
        if (sslKey.isNull()) {
            keyHandle.seek(0);
            sslKey = QSslKey(&keyHandle, QSsl::Ec, QSsl::Pem, QSsl::PrivateKey);
        }
        if (sslKey.isNull())
            return failWith(QStringLiteral("не читается приватный ключ: %1").arg(keyFile));

        QString matchError;
        if (!keyMatchesCert(certFile, keyFile, &matchError))
            return failWith(matchError);

        material.certPath = certFile;
        material.keyPath = keyFile;
        material.caPath.clear();
        material.expires = certExpiry(cert);
        material.ok = true;
        m_last = material;
        return material;
    }

    const QString dir = tlsDirectory();
    if (!ensureDirectory(dir, &material.error))
        return failWith(material.error);

    const QString caCertFile = caCertPath();
    const QString caKeyFile = caKeyPath();
    const QString leafCertFile = certPath();
    const QString leafKeyFile = keyPath();

    // Ключи — самый чувствительный файл приложения: симлинк на ca.key
    // (запись в ~/.config = тот же uid) увёл бы и openssl, и chmod по чужому
    // пути. Отказываемся, не пытаясь «починить» (L-2).
    for (const QString &path : {caCertFile, caKeyFile, leafCertFile, leafKeyFile}) {
        if (QFileInfo(path).isSymLink())
            return failWith(QStringLiteral("%1 — символическая ссылка, отказываюсь использовать")
                                .arg(path));
    }

    // --- Root CA: создаётся один раз, продлевается только сам по себе --------
    bool caReady = false;
    if (QFileInfo::exists(caCertFile) && QFileInfo::exists(caKeyFile)) {
        const QSslCertificate caCert = loadCert(caCertFile);
        caReady = !caCert.isNull() && !expiresWithin(certExpiry(caCert), kCaRenewDays)
                  && caCert.subjectInfo(QSslCertificate::CommonName).value(0)
                         == QLatin1String(kCaCommonName);
        if (caReady) {
            // Соответствие пары проверяем и для готового CA: краш между двумя
            // rename'ами не должен оставить новый ключ со старым сертификатом.
            QString matchError;
            if (!keyMatchesCert(caCertFile, caKeyFile, &matchError))
                caReady = false;
        }
    }

    bool caRegenerated = false;
    if (!caReady) {
        // Пишем в *.tmp и подменяем атомарно: старые ca.key/ca.crt остаются
        // рабочими, пока новые не готовы (M-5).
        const QString caKeyTmp = caKeyFile + QStringLiteral(".tmp");
        const QString caCertTmp = caCertFile + QStringLiteral(".tmp");
        QFile::remove(caKeyTmp);
        QFile::remove(caCertTmp);

        QString opensslError;
        if (!runOpenSsl({QStringLiteral("req"), QStringLiteral("-x509"),
                         QStringLiteral("-newkey"), QStringLiteral("rsa:4096"),
                         QStringLiteral("-sha256"), QStringLiteral("-days"),
                         QString::number(kCaDays), QStringLiteral("-nodes"),
                         QStringLiteral("-keyout"), caKeyTmp, QStringLiteral("-out"), caCertTmp,
                         QStringLiteral("-subj"),
                         QStringLiteral("/CN=%1").arg(QLatin1String(kCaCommonName)),
                         QStringLiteral("-addext"),
                         QStringLiteral("basicConstraints=critical,CA:TRUE"),
                         QStringLiteral("-addext"),
                         QStringLiteral("keyUsage=critical,keyCertSign,cRLSign")},
                        dir, &opensslError)) {
            QFile::remove(caKeyTmp);
            QFile::remove(caCertTmp);
            return failWith(QStringLiteral("не удалось создать CA: %1").arg(opensslError));
        }
        makePrivate(caKeyTmp);

        if (!keyMatchesCert(caCertTmp, caKeyTmp, &opensslError)) {
            QFile::remove(caKeyTmp);
            QFile::remove(caCertTmp);
            return failWith(QStringLiteral("CA не прошёл проверку ключа: %1").arg(opensslError));
        }
        QString renameError;
        if (!replaceFile(caKeyTmp, caKeyFile, &renameError)
            || !replaceFile(caCertTmp, caCertFile, &renameError)) {
            QFile::remove(caKeyTmp);
            QFile::remove(caCertTmp);
            return failWith(renameError);
        }
        caRegenerated = true;
        // Новый CA — старый лист ему не принадлежит; старые файлы не удаляем,
        // их перезапишет атомарная генерация листа ниже.
    }

    const QSslCertificate caCert = loadCert(caCertFile);
    if (caCert.isNull())
        return failWith(QStringLiteral("не читается %1").arg(caCertFile));

    // --- Листовой сертификат --------------------------------------------------
    const QSet<QString> ips = desiredIps(addresses);
    if (ips.isEmpty() && dnsName.trimmed().isEmpty())
        return failWith(QStringLiteral("нет ни одного адреса для SAN — сертификат бесполезен"));

    // invalidateLeaf() больше не удаляет файлы, а только просит перевыпуск.
    const bool forceLeaf = std::exchange(m_renewLeaf, false) || caRegenerated;

    bool leafReady = false;
    if (!forceLeaf && QFileInfo::exists(leafCertFile) && QFileInfo::exists(leafKeyFile)) {
        const QSslCertificate leaf = loadCert(leafCertFile);
        if (!leaf.isNull()) {
            const bool fresh = !expiresWithin(certExpiry(leaf), kLeafRenewDays);
            const bool sameIps = certIps(leaf) == ips;
            const QStringList wantDns = dnsName.trimmed().isEmpty()
                                            ? QStringList()
                                            : QStringList{dnsName.trimmed().toLower()};
            const bool sameDns = certDnsNames(leaf) == wantDns;
            const bool sameIssuer =
                leaf.issuerInfo(QSslCertificate::CommonName).value(0)
                == QLatin1String(kCaCommonName);
            leafReady = fresh && sameIps && sameDns && sameIssuer;
            if (leafReady) {
                // Соответствие ключа и сертификата обязательно и в режиме
                // internal, а не только Files (M-5).
                QString matchError;
                if (!keyMatchesCert(leafCertFile, leafKeyFile, &matchError))
                    leafReady = false;
            }
        }
    }

    if (!leafReady) {
        const QString keyTmp = leafKeyFile + QStringLiteral(".tmp");
        const QString certTmp = leafCertFile + QStringLiteral(".tmp");
        const QString csrFile = QDir(dir).filePath(QStringLiteral("server.csr.tmp"));
        const QString extFile = QDir(dir).filePath(QStringLiteral("server-ext.cnf.tmp"));
        QFile::remove(keyTmp);
        QFile::remove(certTmp);

        QFile ext(extFile);
        if (!ext.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return failWith(QStringLiteral("не удалось записать %1").arg(extFile));
        }
        QByteArray extText;
        extText += "basicConstraints=critical,CA:FALSE\n";
        extText += "keyUsage=critical,digitalSignature,keyEncipherment\n";
        extText += "extendedKeyUsage=serverAuth\n";
        extText += "subjectAltName=" + sanValue(ips, dnsName).toUtf8() + "\n";
        ext.write(extText);
        ext.close();

        const QString commonName = !dnsName.trimmed().isEmpty()
                                       ? dnsName.trimmed().left(64)
                                       : (*ips.constBegin()).left(64);

        QString opensslError;
        if (!runOpenSsl({QStringLiteral("req"), QStringLiteral("-new"),
                         QStringLiteral("-newkey"), QStringLiteral("rsa:2048"),
                         QStringLiteral("-nodes"), QStringLiteral("-sha256"),
                         QStringLiteral("-keyout"), keyTmp, QStringLiteral("-out"), csrFile,
                         QStringLiteral("-subj"), QStringLiteral("/CN=%1").arg(commonName)},
                        dir, &opensslError)) {
            QFile::remove(extFile);
            QFile::remove(csrFile);
            QFile::remove(keyTmp);
            return failWith(QStringLiteral("не удалось создать запрос: %1").arg(opensslError));
        }
        makePrivate(keyTmp);

        const bool signedOk = runOpenSsl(
            {QStringLiteral("x509"), QStringLiteral("-req"), QStringLiteral("-in"), csrFile,
             QStringLiteral("-CA"), caCertFile, QStringLiteral("-CAkey"), caKeyFile,
             QStringLiteral("-set_serial"), QString::fromLatin1(randomSerial()),
             QStringLiteral("-out"), certTmp, QStringLiteral("-days"),
             QString::number(kLeafDays), QStringLiteral("-sha256"), QStringLiteral("-extfile"),
             extFile},
            dir, &opensslError);

        QFile::remove(extFile);
        QFile::remove(csrFile);
        if (!signedOk) {
            QFile::remove(keyTmp);
            QFile::remove(certTmp);
            return failWith(QStringLiteral("не удалось подписать сертификат: %1").arg(opensslError));
        }

        if (!keyMatchesCert(certTmp, keyTmp, &opensslError)) {
            QFile::remove(keyTmp);
            QFile::remove(certTmp);
            return failWith(QStringLiteral("лист не прошёл проверку ключа: %1").arg(opensslError));
        }
        QString renameError;
        // Сначала ключ, потом сертификат: краш между ними оставит новый ключ
        // со старым сертификатом, что поймает keyMatchesCert на следующем ensure().
        if (!replaceFile(keyTmp, leafKeyFile, &renameError)
            || !replaceFile(certTmp, leafCertFile, &renameError)) {
            QFile::remove(keyTmp);
            QFile::remove(certTmp);
            return failWith(renameError);
        }
    }

    const QSslCertificate leaf = loadCert(leafCertFile);
    if (leaf.isNull())
        return failWith(QStringLiteral("не читается %1").arg(leafCertFile));

    material.certPath = leafCertFile;
    material.keyPath = leafKeyFile;
    material.caPath = caCertFile;
    material.expires = certExpiry(leaf);
    material.ok = true;
    m_last = material;
    return material;
}

bool TlsManager::exportCa(const QString &target, QString *error)
{
    const QString source = caCertPath();
    if (!QFileInfo::exists(source)) {
        if (error)
            *error = QStringLiteral("CA ещё не создан: %1").arg(source);
        return false;
    }
    if (target.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("не задан файл назначения");
        return false;
    }
    QFile::remove(target); // QFile::copy не перезаписывает существующий файл
    if (!QFile::copy(source, target)) {
        if (error)
            *error = QStringLiteral("не удалось скопировать %1 в %2").arg(source, target);
        return false;
    }
    return true;
}

void TlsManager::invalidateLeaf()
{
    // Файлы НЕ удаляем: старый лист остаётся рабочим, пока новый не будет
    // сгенерирован и атомарно подменён (M-5). Следующий ensure() перевыпустит
    // лист на том же CA.
    m_renewLeaf = true;
    m_last = TlsMaterial();
}

void TlsManager::setFilePaths(const QString &cert, const QString &key)
{
    m_filesCert = cert;
    m_filesKey = key;
}
