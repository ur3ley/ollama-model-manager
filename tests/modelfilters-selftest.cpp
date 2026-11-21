// Offline self-test for the hub filter logic (no GUI, no network).
//
//   ./modelfilters-selftest     → PASS/FAIL lines, exit 0/1

#include "modelfilters.h"

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
} // namespace

int main()
{
    check(filterlogic::hasCapabilities({"tools", "thinking"}, {}),
          QStringLiteral("no requirements → match"));
    check(filterlogic::hasCapabilities({"tools", "vision"}, {"vision"}),
          QStringLiteral("single capability present"));
    check(!filterlogic::hasCapabilities({"tools"}, {"vision"}),
          QStringLiteral("missing capability → no match"));
    check(filterlogic::hasCapabilities({"Vision"}, {"vision"}),
          QStringLiteral("capability match is case-insensitive"));
    check(!filterlogic::hasCapabilities({"tools"}, {"tools", "vision"}),
          QStringLiteral("all required capabilities must be present"));

    check(filterlogic::largestSizeB({"0.6b", "235b"}) == 235,
          QStringLiteral("largest size parsed"));
    check(filterlogic::largestSizeB({"2b", "27b"}) == 27,
          QStringLiteral("largest size, millions/billions mix"));
    check(filterlogic::largestSizeB({"500m"}) == 1,
          QStringLiteral("millions rounded up to billions"));
    check(filterlogic::largestSizeB({}) == 0, QStringLiteral("empty sizes"));

    check(filterlogic::fitsMaxParams({"0.6b", "235b"}, 8),
          QStringLiteral("small variant fits the limit"));
    check(!filterlogic::fitsMaxParams({"27b", "35b"}, 14),
          QStringLiteral("no variant fits the limit"));
    check(filterlogic::fitsMaxParams({}, 8),
          QStringLiteral("unknown size is not hidden"));
    check(filterlogic::fitsMaxParams({"235b"}, 0),
          QStringLiteral("no limit → everything fits"));

    const QStringList caps = filterlogic::hfTagsToCapabilities(
        {"gguf", "text-generation", "image-text-to-text", "audio-text-to-text",
         "feature-extraction", "text-to-image", "text-to-video", "reasoning"});
    check(caps.contains("vision"), QStringLiteral("hf: vision from image-text-to-text"));
    check(caps.contains("audio"), QStringLiteral("hf: audio"));
    check(caps.contains("embedding"), QStringLiteral("hf: embedding"));
    check(caps.contains("imagegen"), QStringLiteral("hf: image generation"));
    check(caps.contains("videogen"), QStringLiteral("hf: video generation"));
    check(caps.contains("text"), QStringLiteral("hf: text generation"));
    check(caps.contains("thinking"), QStringLiteral("hf: reasoning"));

    check(!filterlogic::hfTagsToCapabilities({"gguf"}).contains("vision"),
          QStringLiteral("hf: plain gguf has no vision"));

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
