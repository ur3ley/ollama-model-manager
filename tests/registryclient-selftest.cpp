// Offline self-test for the ollama.com HTML parsers (search + tags pages).
//
//   ./registryclient-selftest     → PASS/FAIL lines, exit 0/1

#include "registryclient.h"

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

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Two cards copied from the real page (whitespace trimmed).
    const QString searchHtml = QStringLiteral(R"HTML(
<ul>
  <li><a href="/library/qwen3.8" class="group grid">
    <h2 title="qwen3.8"><span>qwen3.8</span></h2>
    <span>2w</span>
    <div><span>tools</span><span>thinking</span><span>vision</span></div>
    <p class="mt-1 line-clamp-2 text-[15px] leading-6">Qwen3.8 &amp; friends: a family of models.</p>
    <span>27b</span><span>35b</span>
  </a></li>
  <li><a href="/library/qwen3" class="group grid">
    <h2 title="qwen3"><span>qwen3</span></h2>
    <span>1y</span>
    <div><span>tools</span><span>thinking</span></div>
    <p class="mt-1 line-clamp-2 text-[15px] leading-6">Qwen3 is the latest generation.</p>
    <span>0.6b</span><span>235b</span>
  </a></li>
</ul>
)HTML");

    const auto models = RegistryClient::parseSearch(searchHtml);
    check(models.size() == 2, QStringLiteral("search: two models"));
    check(models.value(0).name == QLatin1String("qwen3.8"), QStringLiteral("search: name with a dot"));
    check(models.value(0).sizes == QStringList({QStringLiteral("27b"), QStringLiteral("35b")}),
          QStringLiteral("search: sizes"));
    check(models.value(0).capabilities.contains(QLatin1String("vision")),
          QStringLiteral("search: capabilities"));
    check(models.value(0).description.contains(QLatin1String("Qwen3.8 & friends")),
          QStringLiteral("search: description with an entity"));
    check(models.value(1).name == QLatin1String("qwen3"), QStringLiteral("search: second model"));
    check(!models.value(1).capabilities.contains(QLatin1String("vision")),
          QStringLiteral("search: no vision for qwen3"));

    // Duplicate links must not duplicate rows.
    const auto deduped = RegistryClient::parseSearch(
        QStringLiteral(R"HTML(<a href="/library/gemma4">x</a><a href="/library/gemma4">y</a>)HTML"));
    check(deduped.size() == 1, QStringLiteral("search: duplicate links collapsed"));

    // Tag cards have two anchors per row and mixed size/context/input text.
    const QString tagsHtml = QStringLiteral(R"HTML(
<div>
  <a href="/library/qwen3:latest" class="md:hidden">
    <span>qwen3:latest</span><span>500a1f067a9f</span>
  </a>
  <a href="/library/qwen3:latest" class="hidden md:grid">
    <span>qwen3:latest</span><span>5.2GB</span><span>40K</span><span>Text</span>
    <span>500a1f067a9f</span><span>&nbsp;·&nbsp;1 year ago</span>
  </a>
</div>
<div>
  <a href="/library/qwen3:30b-a3b-instruct-2507-q4_K_M" class="hidden md:grid">
    <span>qwen3:30b-a3b-instruct-2507-q4_K_M</span><span>18GB</span><span>256K</span><span>Text</span>
  </a>
</div>
<div>
  <a href="/library/qwen3-vl:8b" class="hidden md:grid">
    <span>qwen3-vl:8b</span><span>6.1GB</span><span>256K</span><span>Vision</span>
  </a>
</div>
)HTML");

    const auto tags = RegistryClient::parseTags(tagsHtml);
    check(tags.size() == 3, QStringLiteral("tags: duplicates collapsed"));
    check(tags.value(0).name == QLatin1String("qwen3:latest"), QStringLiteral("tags: name"));
    check(tags.value(0).size == QLatin1String("5.2GB"), QStringLiteral("tags: size"));
    check(tags.value(0).context == QLatin1String("40K"), QStringLiteral("tags: context"));
    check(tags.value(0).input == QLatin1String("Text"), QStringLiteral("tags: input"));
    check(tags.value(1).context == QLatin1String("256K"), QStringLiteral("tags: context 256K"));
    check(tags.value(2).input == QLatin1String("Vision"), QStringLiteral("tags: vision input"));

    check(RegistryClient::parseSearch(QString()).isEmpty(), QStringLiteral("search: empty page"));
    check(RegistryClient::parseTags(QString()).isEmpty(), QStringLiteral("tags: empty page"));

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
