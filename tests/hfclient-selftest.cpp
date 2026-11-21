// Offline self-test for the HuggingFace Hub client helpers: JSON parsing,
// GGUF file classification and Ollama name generation. No network access.
//
//   ./hfclient-selftest        → PASS/FAIL lines, exit 0/1

#include "hfclient.h"

#include <QCoreApplication>
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

void checkEqual(const QString &actual, const QString &expected, const QString &what)
{
    const bool ok = actual == expected;
    QTextStream out(stdout);
    out << (ok ? "PASS " : "FAIL ") << what;
    if (!ok)
        out << " (got '" << actual << "', expected '" << expected << "')";
    out << "\n";
    if (!ok)
        ++g_failures;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // --- /api/models?search=... -------------------------------------------------
    const QByteArray searchJson = R"([
      {"id": "unsloth/Qwen3-30B-A3B-Instruct-2507-GGUF", "downloads": 494695, "likes": 327},
      {"id": "Qwen/Qwen3-30B-A3B-GGUF", "downloads": 147722}
    ])";
    const auto repos = HfClient::parseSearch(searchJson);
    check(repos.size() == 2, QStringLiteral("search: two repositories"));
    checkEqual(repos.value(0).id, QStringLiteral("unsloth/Qwen3-30B-A3B-Instruct-2507-GGUF"),
               QStringLiteral("search: repository id"));
    check(repos.value(0).downloads == 494695, QStringLiteral("search: downloads"));
    check(repos.value(0).likes == 327, QStringLiteral("search: likes"));
    check(repos.value(1).likes == 0, QStringLiteral("search: missing likes default to 0"));

    check(HfClient::parseSearch("not json").isEmpty(), QStringLiteral("search: garbage rejected"));
    check(HfClient::parseSearch("{}").isEmpty(), QStringLiteral("search: object rejected"));

    // --- /api/models/<repo>?blobs=true ------------------------------------------
    const QByteArray filesJson = R"({
      "id": "unsloth/Qwen3-30B-A3B-Instruct-2507-GGUF",
      "siblings": [
        {"rfilename": ".gitattributes"},
        {"rfilename": "README.md", "size": 1234},
        {"rfilename": "Qwen3-30B-A3B-Instruct-2507-Q4_K_M.gguf", "size": 18600000000},
        {"rfilename": "Qwen3-30B-A3B-Instruct-2507-UD-Q4_K_XL.gguf", "size": 17700000000},
        {"rfilename": "Qwen3-30B-A3B-Instruct-2507-IQ4_XS.gguf", "size": 16400000000},
        {"rfilename": "mmproj-Qwen3-VL-8B-Q8_0.gguf", "size": 900000000},
        {"rfilename": "Qwen3-30B-A3B-Q8_0-00001-of-00002.gguf", "size": 17000000000},
        {"rfilename": "Qwen3-30B-A3B-Q8_0-00002-of-00002.gguf", "size": 16000000000}
      ]
    })";
    const auto files = HfClient::parseFiles(filesJson);
    check(files.size() == 6, QStringLiteral("files: only .gguf entries kept"));

    bool sawQ4KM = false, sawUd = false, sawIq = false, sawSplit = false, sawProjector = false;
    for (const auto &f : files) {
        if (f.name.endsWith(QLatin1String("Q4_K_M.gguf")) && !f.name.contains(QLatin1String("UD")))
            sawQ4KM = f.quant == QLatin1String("Q4_K_M") && !f.split;
        else if (f.name.contains(QLatin1String("UD-Q4_K_XL")))
            sawUd = f.quant == QLatin1String("UD-Q4_K_XL");
        else if (f.name.contains(QLatin1String("IQ4_XS")))
            sawIq = f.quant == QLatin1String("IQ4_XS");
        else if (f.split)
            sawSplit = f.part == 1 || f.part == 2;
        else if (f.name.contains(QLatin1String("mmproj")))
            sawProjector = true;
    }
    check(sawQ4KM, QStringLiteral("files: Q4_K_M recognised"));
    check(sawUd, QStringLiteral("files: UD-Q4_K_XL recognised as one quant"));
    check(sawIq, QStringLiteral("files: IQ4_XS recognised"));
    check(sawSplit, QStringLiteral("files: split parts recognised"));
    check(sawProjector, QStringLiteral("files: mmproj present in the list (UI marks it)"));

    const auto splitFile = HfClient::classifyFile(
        QStringLiteral("Qwen3-30B-A3B-Q8_0-00002-of-00003.gguf"), 100);
    check(splitFile.split && splitFile.part == 2 && splitFile.parts == 3,
          QStringLiteral("classify: split part 2 of 3"));

    // --- Ollama model names -----------------------------------------------------
    checkEqual(HfClient::modelNameFor(
                   QStringLiteral("unsloth/Qwen3-30B-A3B-Instruct-2507-GGUF"),
                   QStringLiteral("Qwen3-30B-A3B-Instruct-2507-UD-Q4_K_XL.gguf")),
               QStringLiteral("qwen3-30b-a3b-instruct-2507:ud-q4_k_xl"),
               QStringLiteral("name: unsloth repo + UD quant"));
    checkEqual(HfClient::modelNameFor(
                   QStringLiteral("Qwen/Qwen3-30B-A3B-GGUF"),
                   QStringLiteral("Qwen3-30B-A3B-Q8_0.gguf")),
               QStringLiteral("qwen3-30b-a3b:q8_0"),
               QStringLiteral("name: Qwen repo + Q8_0"));
    checkEqual(HfClient::modelNameFor(
                   QStringLiteral("mradermacher/Huihui-Qwen3-30B-A3B-Instruct-2507-abliterated-GGUF"),
                   QStringLiteral("Huihui-Qwen3-30B-A3B-Instruct-2507-abliterated.Q4_K_M.gguf")),
               QStringLiteral("huihui-qwen3-30b-a3b-instruct-2507-abliterated:q4_k_m"),
               QStringLiteral("name: no double dash, quant at the end"));

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
