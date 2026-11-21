#include "modelfilters.h"

#include <cmath>

namespace filterlogic {

bool hasCapabilities(const QStringList &caps, const QStringList &required)
{
    for (const QString &want : required) {
        bool found = false;
        for (const QString &have : caps) {
            if (have.compare(want, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

int largestSizeB(const QStringList &sizes)
{
    int largest = 0;
    for (const QString &size : sizes) {
        QString value = size.trimmed();
        if (value.isEmpty())
            continue;
        const QChar suffix = value.back().toLower();
        if (suffix != QLatin1Char('b') && suffix != QLatin1Char('m'))
            continue;
        value.chop(1);
        bool ok = false;
        const double billions = value.toDouble(&ok);
        if (!ok)
            continue;
        const int whole = suffix == QLatin1Char('m')
                ? qMax(1, int(std::ceil(billions / 1000.0)))
                : int(std::ceil(billions));
        largest = qMax(largest, whole);
    }
    return largest;
}

bool fitsMaxParams(const QStringList &sizes, int maxB)
{
    if (maxB <= 0)
        return true;
    if (sizes.isEmpty())
        return true; // unknown size: do not hide the model
    for (const QString &size : sizes) {
        QString value = size.trimmed();
        if (value.isEmpty())
            continue;
        const QChar suffix = value.back().toLower();
        value.chop(1);
        bool ok = false;
        const double billions = value.toDouble(&ok);
        if (!ok)
            continue;
        const double asBillions = suffix == QLatin1Char('m') ? billions / 1000.0 : billions;
        if (asBillions <= maxB + 0.001)
            return true;
    }
    return false;
}

QStringList hfTagsToCapabilities(const QStringList &tags)
{
    QStringList caps;
    auto add = [&caps](const QString &cap) {
        if (!caps.contains(cap))
            caps.append(cap);
    };

    for (const QString &tag : tags) {
        const QString t = tag.toLower();
        if (t == QLatin1String("vision") || t == QLatin1String("image-text-to-text")
                || t == QLatin1String("image-to-text") || t.contains(QLatin1String("multimodal")))
            add(QStringLiteral("vision"));
        if (t == QLatin1String("audio") || t.contains(QLatin1String("audio"))
                || t.contains(QLatin1String("speech")))
            add(QStringLiteral("audio"));
        if (t == QLatin1String("feature-extraction") || t.contains(QLatin1String("sentence-similarity")))
            add(QStringLiteral("embedding"));
        if (t == QLatin1String("text-to-image") || t == QLatin1String("image-generation"))
            add(QStringLiteral("imagegen"));
        if (t == QLatin1String("text-to-video") || t == QLatin1String("video-generation"))
            add(QStringLiteral("videogen"));
        if (t == QLatin1String("text-generation"))
            add(QStringLiteral("text"));
        if (t.contains(QLatin1String("reasoning")))
            add(QStringLiteral("thinking"));
    }
    return caps;
}

} // namespace filterlogic
