#include "hfvariants.h"

#include "quantmatch.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

bool isProjectorName(const QString &name)
{
    return name.contains(QLatin1String("mmproj"), Qt::CaseInsensitive);
}

// `…-MTP-draft.gguf` is a speculative-decoding draft, not a chat model: Ollama
// cannot use it on its own, so it must not become the default pick.
bool isDraftName(const QString &name)
{
    return name.contains(QLatin1String("draft"), Qt::CaseInsensitive);
}

// `Q4_K_M/model-Q4_K_M-00001-of-00003.gguf` -> base `Q4_K_M/model-Q4_K_M`,
// part 1, parts 3. Returns false for a plain single-file GGUF.
bool splitSuffix(const QString &name, QString *base, int *part, int *parts)
{
    static const QRegularExpression re(
            QStringLiteral("^(.*)-(\\d{5})-of-(\\d{5})\\.gguf$"),
            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(name);
    if (!match.hasMatch())
        return false;
    if (base)
        *base = match.captured(1);
    if (part)
        *part = match.captured(2).toInt();
    if (parts)
        *parts = match.captured(3).toInt();
    return true;
}

struct Group {
    QString base;      // name without the part suffix (and without .gguf)
    QString quant;
    qint64 size = 0;
    int parts = 0;
    bool split = false;
    bool projector = false;
    bool draft = false;
    QList<QPair<QString, qint64>> entries; // file name + byte count
};

// HfClient's quant classifier misses the bare `Q2_K` / `Q6_K` forms, so a repo
// that only publishes those would show `Qwen3-…-Q2_K.gguf` while all neighbours
// show a short quant. Recover the trailing quant token for the label only.
QString quantFromName(const QString &base)
{
    QString stem = QFileInfo(base).fileName();
    if (stem.endsWith(QLatin1String(".gguf"), Qt::CaseInsensitive))
        stem.chop(5);
    static const QRegularExpression re(QStringLiteral(
        "(?:^|[-_])((?:UD-)?(?:IQ|TQ)\\d+_[A-Z0-9]+"
        "|(?:UD-)?Q\\d+(?:_[A-Z0-9]+)*"
        "|BF16|F16|F32)$"));
    const QRegularExpressionMatch match = re.match(stem);
    return match.hasMatch() ? match.captured(1) : QString();
}

// Distinct variants of the same quant differ by a suffix after it: a repo may
// publish `…-IQ4_XS` and `…-IQ4_XS-MTP`. Showing just the quant makes those
// rows look identical while their sizes differ, so keep the trailing part.
// The token itself comes from quantmatch so the picker label and the generated
// Ollama tag (`iq4_xs-mtp`) cannot drift apart.
QString labelFor(const QString &base, const QString &quant)
{
    const QString fileName = QFileInfo(base).fileName();
    if (quant.isEmpty())
        return fileName;

    const QString label = quantLabelFromName(fileName);
    return label.isEmpty() ? quant : label;
}

// 0 = selectable single file, 1 = split GGUF, 2 = projector, 3 = draft: the
// unusable entries sort after the quants the user can actually download.
int rankOf(const HfVariant &variant)
{
    if (variant.draft)
        return 3;
    if (variant.split)
        return 1;
    return variant.projector ? 2 : 0;
}

} // namespace

QList<HfVariant> groupHfFiles(const QList<HfClient::File> &files)
{
    QList<Group> groups;
    QHash<QString, int> indexByKey;

    for (const HfClient::File &file : files) {
        if (file.name.isEmpty())
            continue;

        const bool projector = isProjectorName(file.name);
        const bool draft = isDraftName(file.name);
        QString base;
        int part = 0;
        int parts = 0;
        const bool split = splitSuffix(file.name, &base, &part, &parts);
        if (!split) {
            base = file.name;
            parts = 1;
        }

        // Split parts of one quant share a key; because the key carries the
        // projector/draft flag, an mmproj or a draft never swallows a quant
        // that happens to sit in the same directory.
        QString key;
        if (split)
            key = (projector ? QStringLiteral("ps|")
                             : draft ? QStringLiteral("ds|") : QStringLiteral("s|"))
                    + base;
        else if (projector)
            key = QStringLiteral("p|") + file.name;
        else if (draft)
            key = QStringLiteral("d|") + file.name;
        else
            key = QStringLiteral("f|") + file.name;

        const auto found = indexByKey.constFind(key);
        if (found != indexByKey.constEnd()) {
            Group &group = groups[found.value()];
            group.size += file.size;
            group.parts = qMax(group.parts, parts);
            if (group.quant.isEmpty())
                group.quant = file.quant;
            bool duplicate = false;
            for (const auto &entry : std::as_const(group.entries)) {
                if (entry.first == file.name) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                group.entries.append(qMakePair(file.name, file.size));
            continue;
        }

        Group group;
        group.base = base;
        group.quant = file.quant;
        if (group.quant.isEmpty())
            group.quant = quantFromName(base);
        group.size = file.size;
        group.parts = parts;
        group.split = split;
        group.projector = projector;
        group.draft = draft;
        group.entries.append(qMakePair(file.name, file.size));
        indexByKey.insert(key, groups.size());
        groups.append(group);
    }

    QList<HfVariant> variants;
    variants.reserve(groups.size());
    for (Group &group : groups) {
        // Part names are zero-padded, so a plain sort puts 00001 first and the
        // first entry is the part whose quant we want to show.
        std::sort(group.entries.begin(), group.entries.end(),
                  [](const QPair<QString, qint64> &a, const QPair<QString, qint64> &b) {
            return a.first < b.first;
        });

        HfVariant variant;
        for (const auto &entry : std::as_const(group.entries)) {
            variant.files.append(entry.first);
            variant.sizes.append(entry.second);
        }
        variant.id = variant.files.value(0);
        variant.quant = group.quant;
        variant.size = group.size;
        variant.parts = group.split ? group.parts : 0;
        variant.split = group.split;
        variant.projector = group.projector;
        variant.draft = group.draft;
        variant.enabled = !group.projector && !group.draft;
        variant.label = labelFor(group.base, group.quant);
        variants.append(variant);
    }

    // Some repositories publish the same quant twice (re-upload, upper/lower
    // case duplicate). Same label and size means the same download for us.
    QList<HfVariant> unique;
    unique.reserve(variants.size());
    QSet<QString> seen;
    for (const HfVariant &variant : std::as_const(variants)) {
        const QString dedupKey = variant.label
                + QLatin1Char('\x1f') + QString::number(variant.size)
                + QLatin1Char('\x1f') + QString::number(rankOf(variant));
        if (seen.contains(dedupKey))
            continue;
        seen.insert(dedupKey);
        unique.append(variant);
    }

    std::stable_sort(unique.begin(), unique.end(),
                     [](const HfVariant &a, const HfVariant &b) {
        const int rankA = rankOf(a);
        const int rankB = rankOf(b);
        if (rankA != rankB)
            return rankA < rankB;
        if (a.size != b.size)
            return a.size < b.size;
        return a.label.localeAwareCompare(b.label) < 0;
    });

    return unique;
}

QString splitWildcardPattern(const QString &firstFileName)
{
    const QString name = QFileInfo(firstFileName).fileName();
    static const QRegularExpression re(
            QStringLiteral("^(.*)-\\d{5}-(of-\\d{5}\\.gguf)$"),
            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = re.match(name);
    if (!match.hasMatch())
        return name;
    // `?????` + the exact series count: a leftover series with the same prefix
    // (`…-MTP-00001-of-00004`) or another shard count never matches (B-13).
    // The extension keeps its case — `*.gguf` missed `.GGUF` files.
    return match.captured(1) + QStringLiteral("-\?\?\?\?\?-") + match.captured(2);
}
