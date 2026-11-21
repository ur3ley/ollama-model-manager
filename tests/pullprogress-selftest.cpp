// Offline self-test for the `ollama pull` progress aggregation (no GUI, no
// network, no Ollama needed).
//
//   ./pullprogress-selftest     → PASS/FAIL lines, exit 0/1

#include "pullprogress.h"

#include <QTextStream>

namespace {

int g_failures = 0;

void check(bool condition, const QString &what)
{
    QTextStream out(stdout);
    out << (condition ? "PASS " : "FAIL ") << what << "\n";
    if (!condition)
        ++g_failures;
}

// `ollama` prints decimal (SI) sizes: "522 MB" is 522'000'000 bytes.
const qint64 GB = 1000ll * 1000ll * 1000ll;
const qint64 MB = 1000ll * 1000ll;

// One redraw of a layer line, the way `ollama pull` prints it.
QString layerLine(const QString &id, int percent, const QString &received,
                  const QString &total, bool withTotal = true)
{
    const QString bar = QStringLiteral("▕███▏");
    if (withTotal)
        return QStringLiteral("pulling %1: %2% %3 %4/%5")
                .arg(id, QString::number(percent), bar, received, total);
    return QStringLiteral("pulling %1: %2% %3 %4")
            .arg(id, QString::number(percent), bar, received, total);
}

void testThreeLayersWithExpectedBytes()
{
    PullProgressParser parser;
    parser.reset();
    parser.setExpectedBytes(3 * GB);

    // Three 1 GB layers, ten steps each. Sizes are exact megabytes, so the
    // expected aggregate percentage is plain integer arithmetic.
    const QList<QString> ids = {QStringLiteral("111aaa"), QStringLiteral("222bbb"),
                                QStringLiteral("333ccc")};
    const qint64 totalMb = 3 * 1000;  // three 1 GB layers, in MB
    QList<int> percents;
    qint64 lastReceived = -1;
    bool monotonic = true;
    bool valuesMatch = true;

    for (int layer = 0; layer < ids.size(); ++layer) {
        for (int step = 0; step <= 10; ++step) {
            const qint64 receivedMb = step * 100;  // 1 GB layer = 1000 MB
            PullProgressParser::Update update;
            if (step == 10) {
                // The final redraw prints the size alone.
                update = parser.feed(layerLine(ids[layer], 100, QStringLiteral("1.0 GB"),
                                               QString(), false));
            } else {
                update = parser.feed(layerLine(ids[layer], step * 10,
                                               QString::number(receivedMb) + QStringLiteral(" MB"),
                                               QStringLiteral("1.0 GB")));
            }
            // A completed layer counts as its full total, not as the last frame.
            const qint64 received = layer * 1000 + (step == 10 ? 1000 : receivedMb);
            const int expectedPercent = int(received * 100 / totalMb);
            if (update.percent != expectedPercent || update.received != received * MB) {
                QTextStream(stdout) << "  layer " << layer << " step " << step
                                    << ": percent " << update.percent << " (expected "
                                    << expectedPercent << "), received " << update.received
                                    << " (expected " << received * MB << ")\n";
                valuesMatch = false;
            }
            if (update.received < lastReceived)
                monotonic = false;
            lastReceived = update.received;
            percents.append(update.percent);
        }
    }

    check(valuesMatch, QStringLiteral("aggregate bytes and percent match the layer sum"));
    check(monotonic, QStringLiteral("percent never jumps back between layers"));
    check(percents.size() == 33 && percents.last() == 100,
          QStringLiteral("finishes at 100% after the last layer"));
    check(percents.first() == 0, QStringLiteral("starts at 0%"));
}

void testWithoutExpectedBytes()
{
    PullProgressParser parser;
    parser.reset();
    // No expected size: the total comes from the layer totals seen so far.
    PullProgressParser::Update update = parser.feed(
        layerLine(QStringLiteral("abc123"), 50, QStringLiteral("512 MB"), QStringLiteral("1.0 GB")));
    check(update.total == GB, QStringLiteral("total from known layer total"));
    check(update.received == 512 * MB, QStringLiteral("received from the line"));
    // 512 MB of 1.0 GB = 51% after integer truncation.
    check(update.percent == 51, QStringLiteral("percent of the only known layer"));

    update = parser.feed(layerLine(QStringLiteral("abc123"), 100,
                                   QStringLiteral("1.0 GB"), QString(), false));
    check(update.percent == 100, QStringLiteral("single layer reaches 100%"));
    check(update.received == update.total, QStringLiteral("completed layer counts as its total"));
}

void testUnknownTotal()
{
    PullProgressParser parser;
    parser.reset();
    PullProgressParser::Update update = parser.feed(QStringLiteral("pulling manifest"));
    check(update.percent == -1, QStringLiteral("manifest stage → percent unknown"));
    check(update.total == 0, QStringLiteral("manifest stage → total unknown"));
    check(update.label == QStringLiteral("pulling manifest"), QStringLiteral("manifest label"));

    // A layer line without any size keeps the total unknown as well.
    update = parser.feed(QStringLiteral("pulling abc123:   0% ▕          ▏"));
    check(update.total == 0, QStringLiteral("no size in the line → total still unknown"));
    check(update.percent == -1, QStringLiteral("no size in the line → percent unknown"));
    check(update.label == QStringLiteral("pulling abc123"), QStringLiteral("layer label"));
}

void testRepeatedLinesDoNotAccumulate()
{
    PullProgressParser parser;
    parser.reset();
    parser.setExpectedBytes(2 * GB);

    const QString line = layerLine(QStringLiteral("deadbeef"), 40,
                                   QStringLiteral("400 MB"), QStringLiteral("1.0 GB"));
    const PullProgressParser::Update first = parser.feed(line);
    for (int i = 0; i < 20; ++i)
        parser.feed(line);
    const PullProgressParser::Update again = parser.feed(line);

    check(first.received == again.received, QStringLiteral("redraws do not add bytes"));
    check(first.percent == again.percent, QStringLiteral("redraws do not change the percent"));
    check(again.received == 400 * MB, QStringLiteral("received stays at the line value"));
}

void testNoisyLines()
{
    PullProgressParser parser;
    parser.reset();
    parser.setExpectedBytes(2 * GB);

    parser.feed(layerLine(QStringLiteral("cafe01"), 25, QStringLiteral("512 MB"),
                          QStringLiteral("2.0 GB")));
    const PullProgressParser::Update before = parser.feed(QStringLiteral("success"));
    const PullProgressParser::Update garbage = parser.feed(QStringLiteral("Lorem ipsum?? 100%"));
    const PullProgressParser::Update empty = parser.feed(QStringLiteral("   "));

    check(before.received == garbage.received, QStringLiteral("garbage line keeps the bytes"));
    check(garbage.percent == before.percent, QStringLiteral("garbage line keeps the percent"));
    check(empty.received == before.received, QStringLiteral("blank line is a no-op"));
    check(before.label == QStringLiteral("success"), QStringLiteral("stage line becomes the label"));
}

void testAnsiAndCarriageReturns()
{
    PullProgressParser parser;
    parser.reset();
    parser.setExpectedBytes(GB);

    const QString redraw = QStringLiteral("\r\x1b[?25l")
            + layerLine(QStringLiteral("beef42"), 30, QStringLiteral("300 MB"),
                        QStringLiteral("1.0 GB"))
            + QStringLiteral("\x1b[K");
    const PullProgressParser::Update update = parser.feed(redraw);
    check(update.received == 300 * MB, QStringLiteral("ANSI escapes and \\r are stripped"));
    // 300 MB of 1.0 GB = 30%.
    check(update.percent == 30, QStringLiteral("percent from the cleaned line"));
    check(update.label == QStringLiteral("pulling beef42"), QStringLiteral("clean label"));
}

void testStagesKeepTheBytes()
{
    PullProgressParser parser;
    parser.reset();
    parser.setExpectedBytes(2 * GB);

    parser.feed(layerLine(QStringLiteral("aaa111"), 100, QStringLiteral("1.0 GB"), QString()));
    PullProgressParser::Update update = parser.feed(QStringLiteral("verifying sha256 digest"));
    check(update.received == GB, QStringLiteral("verifying keeps the downloaded bytes"));
    check(update.percent == 50, QStringLiteral("verifying keeps the percent"));
    check(update.label == QStringLiteral("verifying sha256 digest"),
          QStringLiteral("verifying label"));

    update = parser.feed(QStringLiteral("writing manifest"));
    check(update.label == QStringLiteral("writing manifest"), QStringLiteral("writing label"));
}

void testGluedFramesAndSpinner()
{
    PullProgressParser parser;
    parser.reset();
    // Ollama glues the manifest spinner frames together with cursor moves and no
    // newline, so one physical line may hold a dozen frames.
    const QString glued = QStringLiteral("\x1b[?2026h\x1b[?25l\x1b[1Gpulling manifest ⠋ \x1b[K")
            + QStringLiteral("\x1b[?25h\x1b[?2026l\x1b[?2026h\x1b[?25l\x1b[1Gpulling manifest ⠙ \x1b[K")
            + QStringLiteral("\x1b[?25h\x1b[?2026l\x1b[?2026h\x1b[?25l\x1b[1Gpulling manifest ⠹ \x1b[K");
    const PullProgressParser::Update update = parser.feed(glued);
    check(update.label == QStringLiteral("pulling manifest"),
          QStringLiteral("glued frames keep a short label"));

    // A layer line glued after a stage line must still be understood.
    const PullProgressParser::Update layer = parser.feed(
        QStringLiteral("\x1b[A\x1b[1G") + layerLine(QStringLiteral("7f4030143c1c"), 40,
                                                       QStringLiteral("200 MB"), QStringLiteral("500 MB")));
    check(layer.received == 200 * MB, QStringLiteral("layer after glued frames is parsed"));
    check(layer.label == QStringLiteral("pulling 7f4030143c1c"), QStringLiteral("layer label"));
}

void testMarkComplete()
{
    PullProgressParser parser;
    parser.reset();
    parser.setExpectedBytes(523 * MB);
    // The biggest layer's last frame is overwritten by the next layer's first,
    // so the sum stays a bit below the registry size.
    parser.feed(layerLine(QStringLiteral("aaa111"), 99, QStringLiteral("517 MB"),
                          QStringLiteral("522 MB")));
    const PullProgressParser::Update unfinished = parser.feed(QStringLiteral("verifying sha256 digest"));
    check(unfinished.percent == 98, QStringLiteral("99% of the last layer is 98% overall"));

    const PullProgressParser::Update done = parser.markComplete();
    check(done.percent == 100, QStringLiteral("successful pull ends at 100%"));
    check(done.received == unfinished.received, QStringLiteral("markComplete does not invent bytes"));
}

void testExpectedClamped()
{
    PullProgressParser parser;
    parser.reset();
    parser.setExpectedBytes(GB / 2);
    // Registry size smaller than what the layers actually report.
    const PullProgressParser::Update update = parser.feed(
        layerLine(QStringLiteral("zzz999"), 100, QStringLiteral("1.0 GB"), QString()));
    check(update.percent == 100, QStringLiteral("percent is clamped to 100"));
}

} // namespace

int main()
{
    testThreeLayersWithExpectedBytes();
    testWithoutExpectedBytes();
    testUnknownTotal();
    testRepeatedLinesDoNotAccumulate();
    testNoisyLines();
    testAnsiAndCarriageReturns();
    testStagesKeepTheBytes();
    testGluedFramesAndSpinner();
    testMarkComplete();
    testExpectedClamped();

    QTextStream(stdout) << (g_failures == 0 ? "ALL PASS\n" : "FAILURES\n");
    return g_failures == 0 ? 0 : 1;
}
