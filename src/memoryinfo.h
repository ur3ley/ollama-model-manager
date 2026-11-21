#ifndef MEMORYINFO_H
#define MEMORYINFO_H

#include <QFile>
#include <QString>

// Rough "will this model fit" check: available RAM plus free discrete VRAM.
// On integrated GPUs (Strix Halo and friends) VRAM is carved out of RAM, so the
// two numbers do not double count in practice; on discrete GPUs the free VRAM is
// what actually matters. Good enough for a green/red hint, not for scheduling.
namespace memoryinfo {

inline quint64 memAvailableBytes()
{
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    // procfs reports size 0, so atEnd() is unreliable — read everything.
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray &line : lines) {
        if (!line.startsWith("MemAvailable:"))
            continue;
        const QByteArray digits = line.mid(13).trimmed().split(' ').value(0);
        bool ok = false;
        const quint64 kb = digits.toULongLong(&ok);
        return ok ? kb * 1024ull : 0;
    }
    return 0;
}

inline quint64 memTotalBytes()
{
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    const QList<QByteArray> lines = file.readAll().split('\n');
    for (const QByteArray &line : lines) {
        if (!line.startsWith("MemTotal:"))
            continue;
        const QByteArray digits = line.mid(9).trimmed().split(' ').value(0);
        bool ok = false;
        const quint64 kb = digits.toULongLong(&ok);
        return ok ? kb * 1024ull : 0;
    }
    return 0;
}

inline quint64 vramFreeBytes()
{
    quint64 freeBytes = 0;
    for (int card = 0; card < 4; ++card) {
        const QString base = QStringLiteral("/sys/class/drm/card%1/device/").arg(card);
        QFile totalFile(base + QStringLiteral("mem_info_vram_total"));
        QFile usedFile(base + QStringLiteral("mem_info_vram_used"));
        if (!totalFile.open(QIODevice::ReadOnly) || !usedFile.open(QIODevice::ReadOnly))
            continue;
        bool okTotal = false;
        bool okUsed = false;
        const quint64 total = totalFile.readAll().trimmed().toULongLong(&okTotal);
        const quint64 used = usedFile.readAll().trimmed().toULongLong(&okUsed);
        if (okTotal && okUsed && total > used)
            freeBytes += total - used;
    }
    return freeBytes;
}

inline quint64 availableBytes()
{
    return memAvailableBytes() + vramFreeBytes();
}

// "18GB", "523MB", "1.2 GB", "98 GB" → bytes (0 when unknown).
inline quint64 parseSizeBytes(const QString &text)
{
    QString value = text.trimmed();
    value.remove(QLatin1Char(' '));
    if (value.isEmpty())
        return 0;

    quint64 multiplier = 1;
    if (value.endsWith(QLatin1String("TB"), Qt::CaseInsensitive)) {
        multiplier = 1024ull * 1024ull * 1024ull * 1024ull;
        value.chop(2);
    } else if (value.endsWith(QLatin1String("GB"), Qt::CaseInsensitive)) {
        multiplier = 1024ull * 1024ull * 1024ull;
        value.chop(2);
    } else if (value.endsWith(QLatin1String("MB"), Qt::CaseInsensitive)) {
        multiplier = 1024ull * 1024ull;
        value.chop(2);
    } else if (value.endsWith(QLatin1String("KB"), Qt::CaseInsensitive)) {
        multiplier = 1024ull;
        value.chop(2);
    } else if (value.endsWith(QLatin1String("B"), Qt::CaseInsensitive)) {
        value.chop(1);
    } else {
        return 0;
    }

    bool ok = false;
    const double number = value.toDouble(&ok);
    return ok ? quint64(number * double(multiplier)) : 0;
}

// 15% headroom for the KV cache and runtime overhead.
inline bool fits(quint64 bytes)
{
    if (bytes == 0)
        return true;
    const quint64 available = availableBytes();
    if (available == 0)
        return true;
    return bytes + bytes / 7 <= available;
}

// Three states instead of a hard yes/no: a model can fit into RAM on its own
// yet leave too little for the KV cache, which depends on the context the user
// will pick later. Such a case is a warning, not a refusal.
enum class Fit {
    Unknown,  // no data (or no size) — do not paint red
    Yes,      // fits with the 15% headroom
    Tight,    // fits on its own, but the headroom does not
    No        // larger than the available memory
};

inline Fit fitLevel(quint64 bytes)
{
    if (bytes == 0)
        return Fit::Unknown;
    const quint64 available = availableBytes();
    if (available == 0)
        return Fit::Unknown;
    if (bytes > available)
        return Fit::No;
    if (bytes + bytes / 7 > available)
        return Fit::Tight;
    return Fit::Yes;
}

} // namespace memoryinfo

#endif // MEMORYINFO_H
