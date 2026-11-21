#include "pullprogress.h"

#include <QRegularExpression>

#include <algorithm>

namespace {

// CSI sequences: colour, cursor moves, cursor hide/show around redraws.
const QRegularExpression &ansiRe()
{
    static const QRegularExpression re(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]"));
    return re;
}

// "ESC [ n G" moves the cursor to column n: the start of a redrawn frame.
const QRegularExpression &cursorColumnRe()
{
    static const QRegularExpression re(QStringLiteral("\x1b\\[\\d*G"));
    return re;
}

// Stage lines the CLI prints between layers; the spinner glyphs may follow.
const QRegularExpression &stageRe()
{
    static const QRegularExpression re(QStringLiteral(
        "^(pulling manifest|verifying sha256 digest|writing manifest|success)"));
    return re;
}

// "pulling <id>:  45% …" — the id is everything up to the colon.
const QRegularExpression &layerRe()
{
    static const QRegularExpression re(QStringLiteral("^pulling\\s+(\\S+?):\\s*(\\d+)%"));
    return re;
}

// "1.2 GB / 1.5 GB"
const QRegularExpression &sizePairRe()
{
    static const QRegularExpression re(QStringLiteral(
        "(\\d+(?:\\.\\d+)?\\s*[KMGTP]?B)\\s*/\\s*(\\d+(?:\\.\\d+)?\\s*[KMGTP]?B)"));
    return re;
}

// The 100% redraw prints the final size alone: "… 100% ▕███▏ 1.1 GB".
const QRegularExpression &sizeRe()
{
    static const QRegularExpression re(QStringLiteral("(\\d+(?:\\.\\d+)?\\s*[KMGTP]?B)"));
    return re;
}

} // namespace

qint64 pullprogress::parseSizeBytes(const QString &text)
{
    QString value = text.trimmed();
    value.remove(QLatin1Char(' '));
    if (value.isEmpty())
        return 0;

    qint64 multiplier = 1;
    if (value.endsWith(QLatin1String("TB"), Qt::CaseInsensitive)) {
        multiplier = 1000ll * 1000ll * 1000ll * 1000ll;
        value.chop(2);
    } else if (value.endsWith(QLatin1String("GB"), Qt::CaseInsensitive)) {
        multiplier = 1000ll * 1000ll * 1000ll;
        value.chop(2);
    } else if (value.endsWith(QLatin1String("MB"), Qt::CaseInsensitive)) {
        multiplier = 1000ll * 1000ll;
        value.chop(2);
    } else if (value.endsWith(QLatin1String("KB"), Qt::CaseInsensitive)) {
        multiplier = 1000ll;
        value.chop(2);
    } else if (value.endsWith(QLatin1String("B"), Qt::CaseInsensitive)) {
        value.chop(1);
    } else {
        return 0;
    }

    bool ok = false;
    const double number = value.toDouble(&ok);
    return ok ? qint64(number * double(multiplier)) : 0;
}

void PullProgressParser::reset()
{
    m_expectedBytes = 0;
    m_layers.clear();
    m_label.clear();
    m_complete = false;
}

void PullProgressParser::setExpectedBytes(qint64 bytes)
{
    m_expectedBytes = bytes > 0 ? bytes : 0;
}

PullProgressParser::Update PullProgressParser::feed(const QString &raw)
{
    QString normalized = raw;
    // Frames are redrawn in place with '\r' or "cursor to column N"; Ollama also
    // repositions the cursor and redraws the "pulling manifest" line right after
    // a layer frame, so one physical line may hold frames of two different lines.
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    normalized.replace(cursorColumnRe(), QStringLiteral("\n"));

    QString layerFrame;
    QString stageFrame;
    int stageRank = -1;
    const QStringList frames = normalized.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (QString frame : frames) {
        frame.remove(ansiRe());
        frame = frame.trimmed();
        if (frame.isEmpty())
            continue;
        if (layerRe().match(frame).hasMatch()) {
            // The last layer frame of the line is the freshest.
            layerFrame = frame;
            continue;
        }
        const QRegularExpressionMatch stageMatch = stageRe().match(frame);
        const QString stage = stageMatch.hasMatch() ? stageMatch.captured(1) : frame.left(80);
        // Real stages beat the "pulling manifest" spinner that follows them.
        const int rank = (stage == QLatin1String("pulling manifest") || !stageMatch.hasMatch())
                ? 0 : 1;
        if (rank >= stageRank) {
            stageRank = rank;
            stageFrame = stage;
        }
    }

    if (layerFrame.isEmpty()) {
        if (!stageFrame.isEmpty())
            m_label = stageFrame;
        return snapshot();
    }

    const QString line = layerFrame;
    const QRegularExpressionMatch layerMatch = layerRe().match(line);
    const QString id = layerMatch.captured(1);
    const int linePercent = layerMatch.captured(2).toInt();
    Layer &layer = m_layers[id];

    // Byte counters replace the previous value of this layer, never add to it:
    // the same line is redrawn dozens of times while a layer downloads.
    const QRegularExpressionMatch pairMatch = sizePairRe().match(line);
    if (pairMatch.hasMatch()) {
        layer.received = qint64(pullprogress::parseSizeBytes(pairMatch.captured(1)));
        const qint64 layerTotal = qint64(pullprogress::parseSizeBytes(pairMatch.captured(2)));
        if (layerTotal > 0)
            layer.total = layerTotal;
    } else {
        const QRegularExpressionMatch sizeMatch = sizeRe().match(line);
        if (sizeMatch.hasMatch()) {
            const qint64 size = qint64(pullprogress::parseSizeBytes(sizeMatch.captured(1)));
            if (linePercent >= 100 && size > 0) {
                layer.total = size;
                layer.received = size;
            } else if (size > layer.received) {
                layer.received = size;
            }
        }
    }

    if (linePercent >= 100) {
        layer.done = true;
        if (layer.total <= 0)
            layer.total = layer.received;
        layer.received = layer.total;
    }

    m_label = QStringLiteral("pulling ") + id;
    return snapshot();
}

PullProgressParser::Update PullProgressParser::markComplete()
{
    m_complete = true;
    return snapshot();
}

PullProgressParser::Update PullProgressParser::snapshot() const
{
    Update update;
    qint64 knownTotal = 0;
    for (auto it = m_layers.cbegin(); it != m_layers.cend(); ++it) {
        const Layer &layer = it.value();
        update.received += layer.done ? layer.total : layer.received;
        knownTotal += layer.total;
    }

    update.total = m_expectedBytes > 0 ? m_expectedBytes : knownTotal;
    if (update.total > 0) {
        if (m_complete)
            update.percent = 100;
        else
            update.percent = int(std::clamp<qint64>(update.received * 100 / update.total, 0, 100));
    }
    update.label = m_label;
    return update;
}
