#include "quantmatch.h"

#include <QFileInfo>
#include <QRegularExpression>

namespace {

QString withoutShaPrefix(QString value)
{
    value = value.trimmed();
    const QString prefix = QStringLiteral("sha256:");
    if (value.startsWith(prefix, Qt::CaseInsensitive))
        value = value.mid(prefix.size());
    return value.toLower();
}

} // namespace

QString stripQuantSuffix(const QString &tagPart)
{
    static const QRegularExpression quantRe(
        QStringLiteral("-(?:ud-)?(?:iq[0-9]+(?:_[a-z0-9]+)*|q[0-9]+(?:_[a-z0-9]+)+"
                       "|bf16|f16|f32|fp16|fp8)$"),
        QRegularExpression::CaseInsensitiveOption);
    QString stem = tagPart;
    stem.remove(quantRe);
    return stem;
}

QString bindSearchStem(const QString &modelName)
{
    const QString family = modelName.section(QLatin1Char(':'), 0, 0).trimmed();
    const QString stem = stripQuantSuffix(family);
    return stem.isEmpty() ? family : stem;
}

bool digestMatches(const QString &localId, const QString &registryDigest)
{
    const QString local = withoutShaPrefix(localId);
    const QString remote = withoutShaPrefix(registryDigest);
    if (local.isEmpty() || remote.isEmpty())
        return false;

    // Very short values cannot be told apart by prefix: require equality so a
    // truncated id is never mistaken for an unrelated model.
    if (local.size() < 8 || remote.size() < 8)
        return local == remote;

    return local.startsWith(remote) || remote.startsWith(local);
}

VariantState variantState(bool installed, bool sameAsSource)
{
    if (!installed)
        return VariantState::NotInstalled;
    return sameAsSource ? VariantState::Installed : VariantState::UpdateAvailable;
}

bool shaDiffers(const QString &installedSha, const QString &remoteSha)
{
    const QString local = withoutShaPrefix(installedSha);
    const QString remote = withoutShaPrefix(remoteSha);
    // Nothing recorded on install (or no reply yet): show no update rather than
    // a mark the user cannot act on.
    if (local.isEmpty() || remote.isEmpty())
        return false;

    // Same prefix rule as digestMatches: a short local value must be a prefix
    // of the full remote sha to count as the same commit.
    if (local.size() < 8 || remote.size() < 8)
        return local != remote;

    return !local.startsWith(remote) && !remote.startsWith(local);
}

QString ggufFileNameStem(const QString &fileName)
{
    static const QRegularExpression splitRe(
        QStringLiteral("-\\d{5}-of-\\d{5}\\.gguf$"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression ggufRe(QStringLiteral("\\.gguf$"),
                                           QRegularExpression::CaseInsensitiveOption);

    QString name = QFileInfo(fileName).fileName();
    if (splitRe.match(name).hasMatch()) {
        name.remove(splitRe);
        return name; // the part suffix is followed by .gguf, nothing else to cut
    }
    name.remove(ggufRe);
    return name;
}

bool sameGgufVariant(const QString &left, const QString &right)
{
    if (left.isEmpty() || right.isEmpty())
        return false;
    return ggufFileNameStem(left).compare(ggufFileNameStem(right), Qt::CaseInsensitive) == 0;
}

QString canonicalHfModelName(const QString &repo, const QString &fileName)
{
    QString base = repo.section(QLatin1Char('/'), -1);
    for (const QString &suffix : {QStringLiteral("-GGUF"), QStringLiteral("-gguf")}) {
        if (base.endsWith(suffix))
            base.chop(suffix.size());
    }

    auto sanitize = [](const QString &raw) {
        QString out;
        out.reserve(raw.size());
        for (const QChar ch : raw) {
            const QChar lower = ch.toLower();
            if (lower.isLetterOrNumber() || lower == QLatin1Char('.')
                    || lower == QLatin1Char('_'))
                out.append(lower);
            else if (out.isEmpty() || out.endsWith(QLatin1Char('-')))
                continue;
            else
                out.append(QLatin1Char('-'));
        }
        while (out.endsWith(QLatin1Char('-')))
            out.chop(1);
        return out;
    };

    QString tag = quantLabelFromName(fileName);
    if (tag.isEmpty())
        tag = ggufFileNameStem(fileName);
    if (tag.isEmpty())
        tag = QFileInfo(fileName).completeBaseName();

    return sanitize(base) + QLatin1Char(':') + sanitize(tag);
}

QString quantLabelFromName(const QString &fileName)
{
    // Bare forms (`Q2_K`, `Q6_K`) are missing from HfClient's classifier, so the
    // token is recovered from the name itself. A separator may be any of
    // `-`, `_` or `.` (`…-abliterated.Q4_K_M.gguf` is common).
    static const QRegularExpression quantRe(QStringLiteral(
        "(?:^|[-_.])((?:UD-)?(?:IQ|TQ)\\d+_[A-Za-z0-9]+"
        "|(?:UD-)?Q\\d+(?:_[A-Za-z0-9]+)*"
        "|BF16|F16|F32)"),
        QRegularExpression::CaseInsensitiveOption);

    const QString stem = ggufFileNameStem(fileName);
    QString quant;
    qsizetype end = -1;
    // The last token wins: in `Q4_K_M/model-Q4_K_M-00001…` the quant leads the
    // file name, in `Qwen3-Coder-Q2_K` it trails.
    auto it = quantRe.globalMatch(stem);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        quant = match.captured(1);
        end = match.capturedEnd(1);
    }
    if (quant.isEmpty())
        return QString();

    // Everything after the quant distinguishes otherwise identical rows: a repo
    // may publish `…-IQ4_XS` and `…-IQ4_XS-MTP`, which must not collapse into
    // one label (or one model name).
    QString tail = stem.mid(end);
    while (!tail.isEmpty()
           && (tail.front() == QLatin1Char('-') || tail.front() == QLatin1Char('_')
               || tail.front() == QLatin1Char('.'))) {
        tail.remove(0, 1);
    }
    return tail.isEmpty() ? quant : quant + QLatin1Char('-') + tail;
}
