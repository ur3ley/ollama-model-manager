#include "tokenmanager.h"

#include "tokenmap.h"

#include <QDateTime>
#include <QDebug>
#include <QRandomGenerator>

#include <kwallet.h>

#include "walletopen.h"

#include <utility>

namespace {
const char *kWalletName = "kdewallet";
const char *kFolder = "ollama-auth";
const char *kCreatedPrefix = "created:";
// "admin:<device>" = "1": токен может вызывать административный API (S-3).
const char *kAdminPrefix = "admin:";
} // namespace

TokenManager::TokenManager(QObject *parent)
    : TokenManager(walletFolder(), parent)
{
}

TokenManager::TokenManager(const QString &folder, QObject *parent)
    : QObject(parent)
    , m_folder(folder.isEmpty() ? walletFolder() : folder)
{
}

TokenManager::~TokenManager()
{
    if (m_wallet) {
        delete m_wallet;
    }
}

QString TokenManager::walletName()
{
    return QString::fromLatin1(kWalletName);
}

QString TokenManager::walletFolder()
{
    return QString::fromLatin1(kFolder);
}

QString TokenManager::generateToken()
{
    // 24 random bytes -> 48 hex chars, same format the proxy has always used.
    constexpr int kBytes = 24;
    QByteArray raw;
    raw.reserve(kBytes);
    while (raw.size() < kBytes) {
        const quint32 value = QRandomGenerator::system()->generate();
        for (int i = 0; i < 4 && raw.size() < kBytes; ++i)
            raw.append(static_cast<char>(value >> (8u * static_cast<unsigned>(i))));
    }
    return QString::fromLatin1(raw.toHex());
}

bool TokenManager::isWalletAvailable() const
{
    return KWallet::Wallet::isOpen(QString::fromLatin1(kWalletName));
}

bool TokenManager::walletUnlocked() const
{
    // isOpen() — пассивный запрос к kwalletd: не открывает кошелёк и не
    // показывает запрос пароля. Если заперт, любой вызов openWallet() поднял
    // бы диалог, а демону это недопустимо (он работает без пользователя).
    return KWallet::Wallet::isOpen(walletName());
}

QList<TokenManager::Entry> TokenManager::entriesIfUnlocked(QString *error)
{
    if (!walletUnlocked()) {
        if (error)
            *error = QStringLiteral("wallet '%1' is locked").arg(walletName());
        return {};
    }
    // Кошелёк уже открыт, поэтому openWallet() внутри ensureWallet() пройдёт
    // без запроса пароля.
    return entries(error);
}

bool TokenManager::ensureWallet(QString *error)
{
    if (!KWallet::Wallet::isEnabled()) {
        if (error)
            *error = QStringLiteral("KWallet is not enabled in this session");
        return false;
    }

    if (!m_wallet) {
        // Асинхронно, с локальным циклом (S-10): окно не замирает на время
        // запроса пароля. Таймер reloadTokens может сработать внутри этого
        // цикла — повторно кошелёк не открываем.
        if (m_opening) {
            if (error)
                *error = QStringLiteral("wallet '%1' is being opened").arg(walletName());
            return false;
        }
        m_opening = true;
        m_wallet = walletopen::open(walletName());
        m_opening = false;
        if (!m_wallet) {
            if (error)
                *error = QStringLiteral("cannot open wallet '%1'").arg(walletName());
            return false;
        }
    }

    if (!m_wallet->isOpen()) {
        if (error)
            *error = QStringLiteral("wallet '%1' is closed").arg(walletName());
        return false;
    }

    if (!m_wallet->hasFolder(m_folder) && !m_wallet->createFolder(m_folder)) {
        if (error)
            *error = QStringLiteral("cannot create folder '%1' in wallet").arg(m_folder);
        return false;
    }
    if (!m_wallet->setFolder(m_folder)) {
        if (error)
            *error = QStringLiteral("cannot open folder '%1' in wallet").arg(m_folder);
        return false;
    }
    return true;
}

QList<TokenManager::Entry> TokenManager::entries(QString *error)
{
    QList<Entry> result;
    if (!ensureWallet(error))
        return result;

    const QStringList keys = m_wallet->entryList();
    QStringList devices;
    int skipped = 0;
    for (const QString &key : keys) {
        if (key.startsWith(QString::fromLatin1(kCreatedPrefix))
            || key.startsWith(QString::fromLatin1(kAdminPrefix)))
            continue;
        // В папку может попасть что угодно (старый скрипт, другой инструмент,
        // импорт). Имя уходит в заголовок и в журнал — валидируем при загрузке,
        // как и при выпуске (M-3).
        if (!isValidDeviceName(key)) {
            ++skipped;
            continue;
        }
        devices << key;
    }
    devices.sort(Qt::CaseInsensitive);
    if (skipped > 0) {
        qWarning().noquote() << QStringLiteral("wallet '%1': пропущено %2 записей с недопустимым "
                                               "именем устройства")
                                    .arg(m_folder)
                                    .arg(skipped);
    }

    for (const QString &device : std::as_const(devices)) {
        Entry entry;
        entry.device = device;
        m_wallet->readPassword(device, entry.token);
        m_wallet->readPassword(QString::fromLatin1(kCreatedPrefix) + device, entry.created);
        QString admin;
        if (m_wallet->hasEntry(QString::fromLatin1(kAdminPrefix) + device)
            && m_wallet->readPassword(QString::fromLatin1(kAdminPrefix) + device, admin) == 0)
            entry.admin = admin == QLatin1String("1");
        result << entry;
    }
    return result;
}

bool TokenManager::issue(const QString &device, QString *token, QString *error)
{
    if (!isValidDeviceName(device)) {
        if (error)
            *error = QStringLiteral("invalid device name '%1'").arg(device);
        return false;
    }
    if (!ensureWallet(error))
        return false;

    const QString value = generateToken();
    if (m_wallet->writePassword(device, value) != 0) {
        if (error)
            *error = QStringLiteral("cannot write token to wallet");
        return false;
    }
    if (m_wallet->writePassword(QString::fromLatin1(kCreatedPrefix) + device,
                                QDateTime::currentDateTime().toString(Qt::ISODate)) != 0) {
        if (error)
            *error = QStringLiteral("cannot write creation date to wallet");
        return false;
    }
    if (token)
        *token = value;
    emit tokensChanged();
    return true;
}

bool TokenManager::revoke(const QString &device, QString *error)
{
    if (!ensureWallet(error))
        return false;

    if (!m_wallet->hasEntry(device)) {
        if (error)
            *error = QStringLiteral("no token for '%1'").arg(device);
        return false;
    }
    if (m_wallet->removeEntry(device) != 0) {
        if (error)
            *error = QStringLiteral("cannot remove token from wallet");
        return false;
    }
    m_wallet->removeEntry(QString::fromLatin1(kCreatedPrefix) + device);
    m_wallet->removeEntry(QString::fromLatin1(kAdminPrefix) + device);
    emit tokensChanged();
    return true;
}

bool TokenManager::setAdmin(const QString &device, bool admin, QString *error)
{
    if (!ensureWallet(error))
        return false;
    if (!m_wallet->hasEntry(device)) {
        if (error)
            *error = QStringLiteral("no token for '%1'").arg(device);
        return false;
    }
    const QString key = QString::fromLatin1(kAdminPrefix) + device;
    const int rc = admin ? m_wallet->writePassword(key, QStringLiteral("1"))
                         : (m_wallet->hasEntry(key) ? m_wallet->removeEntry(key) : 0);
    if (rc != 0) {
        if (error)
            *error = QStringLiteral("cannot update admin flag in wallet");
        return false;
    }
    emit tokensChanged();
    return true;
}

bool TokenManager::token(const QString &device, QString *value, QString *error)
{
    if (!ensureWallet(error))
        return false;
    if (!m_wallet->hasEntry(device)) {
        if (error)
            *error = QStringLiteral("no token for '%1'").arg(device);
        return false;
    }
    QString result;
    if (m_wallet->readPassword(device, result) != 0) {
        if (error)
            *error = QStringLiteral("cannot read token from wallet");
        return false;
    }
    if (value)
        *value = std::move(result);
    return true;
}

bool TokenManager::removeFolder(QString *error)
{
    if (!ensureWallet(error))
        return false;
    if (!m_wallet->removeFolder(m_folder)) {
        if (error)
            *error = QStringLiteral("cannot remove folder '%1' from wallet").arg(m_folder);
        return false;
    }
    return true;
}
