#include "tokenmap.h"

#include <QCryptographicHash>
#include <QDebug>

QByteArray TokenMap::hashToken(const QByteArray &token)
{
    return QCryptographicHash::hash(token, QCryptographicHash::Sha256);
}

void TokenMap::setTokens(const QList<QPair<QByteArray, QString>> &tokens)
{
    m_devices.clear();
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

int TokenMap::size() const
{
    return int(m_devices.size());
}

void TokenMap::clear()
{
    m_devices.clear();
}
