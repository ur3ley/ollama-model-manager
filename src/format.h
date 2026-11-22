#ifndef FORMAT_H
#define FORMAT_H

#include <QLatin1String>
#include <QString>

// Number formatting shared by the panels (A-5: four copies had drifted apart).
namespace format {

/** 1536 → "1.5 KB", 0 or negative → `zeroText`. Binary units, one decimal below 10. */
inline QString byteSize(qint64 bytes, const QString &zeroText = QString())
{
    if (bytes <= 0)
        return zeroText;
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QString::number(value, 'f', value < 10.0 ? 1 : 0)
            + QLatin1Char(' ') + QLatin1String(units[unit]);
}

/** Download/like counters: 1234 → "1.2K", 5600000 → "5.6M". */
inline QString compactCount(quint64 value)
{
    if (value >= 1000000000ull)
        return QString::number(value / 1e9, 'f', 1) + QStringLiteral("B");
    if (value >= 1000000ull)
        return QString::number(value / 1e6, 'f', 1) + QStringLiteral("M");
    if (value >= 1000ull)
        return QString::number(value / 1e3, 'f', 1) + QStringLiteral("K");
    return QString::number(value);
}

} // namespace format

#endif // FORMAT_H
