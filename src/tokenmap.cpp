#include "tokenmap.h"

#include <QCryptographicHash>
#include <QDebug>

QByteArray TokenMap::hashToken(const QByteArray &token)
{
    return QCryptographicHash::hash(token, QCryptographicHash::Sha256);
}

void TokenMap::setTokens(const QList<QPair<QByteArray, QString>> &tokens,
                         const QSet<QString> &adminDevices)
{
    m_devices.clear();
    m_admins.clear();
    m_devices.reserve(tokens.size());
    int skipped = 0;
    for (const auto &entry : tokens) {
        const QByteArray &token = entry.first;
        const QString &device = entry.second;
        if (token.isEmpty() || !isValidDeviceName(device)) {
            ++skipped;
            continue;
        }
        m_devices.insert(hashToken(token), device);
        if (adminDevices.contains(device))
            m_admins.insert(device);
    }
    // Одна строка на перезагрузку карты, а не по строке на запись: прокси
    // перечитывает кошелёк раз в минуту.
    if (skipped > 0) {
        qWarning().noquote()
            << QStringLiteral("proxy: пропущено %1 записей токенов с недопустимым именем "
                              "устройства (ожидается ^[A-Za-z0-9._-]{1,32}$)")
                   .arg(skipped);
    }
}

QString TokenMap::deviceForToken(const QByteArray &token) const
{
    if (token.isEmpty())
        return QString();
    return m_devices.value(hashToken(token));
}

bool TokenMap::isAdmin(const QString &device) const
{
    return !device.isEmpty() && m_admins.contains(device);
}

int TokenMap::size() const
{
    return int(m_devices.size());
}

void TokenMap::clear()
{
    m_devices.clear();
    m_admins.clear();
}
