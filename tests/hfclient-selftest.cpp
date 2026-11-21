// Offline self-test for the HuggingFace Hub client helpers: JSON parsing,
// GGUF file classification and Ollama name generation. No network access.
//
//   ./hfclient-selftest        → PASS/FAIL lines, exit 0/1

#include "hfclient.h"

#include <QCoreApplication>
#include <QNetworkRequest>
#include <QTextStream>
#include <QUrl>

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

    // Two quants of one repository that differ only by the suffix after the
    // quant must get different names, or installing the second would overwrite
    // the first (the `IQ4_XS` / `IQ4_XS-MTP` bug).
    checkEqual(HfClient::modelNameFor(
                   QStringLiteral("orcarouter/Qwen3.8-Flash-Next-Uncensored-GGUF"),
                   QStringLiteral("Qwen3.8-Flash-Next-Uncensored-IQ4_XS.gguf")),
               QStringLiteral("qwen3.8-flash-next-uncensored:iq4_xs"),
               QStringLiteral("name: plain IQ4_XS"));
    checkEqual(HfClient::modelNameFor(
                   QStringLiteral("orcarouter/Qwen3.8-Flash-Next-Uncensored-GGUF"),
                   QStringLiteral("Qwen3.8-Flash-Next-Uncensored-IQ4_XS-MTP-00001-of-00003.gguf")),
               QStringLiteral("qwen3.8-flash-next-uncensored:iq4_xs-mtp"),
               QStringLiteral("name: split MTP quant keeps its suffix"));
    checkEqual(HfClient::modelNameFor(
                   QStringLiteral("unsloth/Qwen3-30B-A3B-GGUF"),
                   QStringLiteral("Qwen3-30B-A3B-Q8_0-MTP-00001-of-00002.gguf")),
               QStringLiteral("qwen3-30b-a3b:q8_0-mtp"),
               QStringLiteral("name: split Q8_0-MTP"));
    checkEqual(HfClient::modelNameFor(
                   QStringLiteral("unsloth/Qwen3-30B-A3B-GGUF"),
                   QStringLiteral("Qwen3-30B-A3B-Q2_K.gguf")),
               QStringLiteral("qwen3-30b-a3b:q2_k"),
               QStringLiteral("name: bare Q2_K recovered from the file name"));

    // --- gated repositories: token -> Authorization header ----------------------
    checkEqual(HfClient::normalizeToken(QStringLiteral("  hf_abc123 \n")),
               QStringLiteral("hf_abc123"),
               QStringLiteral("token: spaces and line breaks trimmed"));
    checkEqual(HfClient::normalizeToken(QStringLiteral("\t\r\n ")), QString(),
               QStringLiteral("token: whitespace-only becomes empty"));

    HfClient client;
    check(!client.hasToken(), QStringLiteral("token: anonymous by default"));
    client.setToken(QStringLiteral("  hf_x \r\n"));
    checkEqual(client.token(), QStringLiteral("hf_x"),
               QStringLiteral("token: setToken trims what the user typed"));
    client.setToken(QStringLiteral("   "));
    check(!client.hasToken(), QStringLiteral("token: blank input clears the header"));

    const QUrl gatedUrl(QStringLiteral("https://huggingface.co/api/models/meta-llama/Llama-3-GGUF"));
    const QNetworkRequest anonymous = HfClient::requestFor(gatedUrl, QString());
    check(!anonymous.hasRawHeader("Authorization"),
          QStringLiteral("request: no Authorization header without a token"));
    check(anonymous.rawHeader("User-Agent") == QByteArrayLiteral("ollama-model-manager"),
          QStringLiteral("request: user agent kept"));
    check(anonymous.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt()
              == int(QNetworkRequest::NoLessSafeRedirectPolicy),
          QStringLiteral("request: redirect policy kept"));

    const QNetworkRequest authorized =
            HfClient::requestFor(gatedUrl, QStringLiteral(" hf_secret\n"));
    check(authorized.rawHeader("Authorization") == QByteArrayLiteral("Bearer hf_secret"),
          QStringLiteral("request: Bearer header from a pasted token"));
    check(authorized.url() == gatedUrl, QStringLiteral("request: url unchanged"));

    check(HfClient::isAuthError(QStringLiteral("HTTP 401: this repository is gated, a token is required")),
          QStringLiteral("error: 401 recognised (hint is added by the panel)"));
    check(!HfClient::isAuthError(QStringLiteral("HTTP 404: repository or file not found")),
          QStringLiteral("error: 404 is not a token problem"));

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
