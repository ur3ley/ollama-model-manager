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
  <li><a href="/h4rithd/nullguard" class="group grid grid-cols-1 gap-x-4 px-4 py-6 sm:grid-cols-[minmax(0,1fr)_auto] md:px-7">
    <h2 title="nullguard"><span >h4rithd/nullguard</span></h2>
    <span title="Updated May 12, 2026">6mo</span>
    <div><span class="inline-flex bg-indigo-50">tools</span><span class="inline-flex bg-indigo-50">thinking</span></div>
    <p class="mt-1 line-clamp-2 text-[15px] leading-6 text-black sm:col-span-2">NullGuard is an abliterated research build.</p>
    <span title="614 downloads"><span>614</span></span>
  </a></li>
</ul>
<p><a href="/public/icon-32x32.png">icon</a><a href="/download">download</a></p>
)HTML");

    const auto models = RegistryClient::parseSearch(searchHtml);
    check(models.size() == 3, QStringLiteral("search: three models, nav links ignored"));
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

    // Namespaced card: the ref keeps its owner, the chips come from the same
    // markup as the official ones and the download count is the title pair.
    check(models.value(2).name == QLatin1String("h4rithd/nullguard"),
          QStringLiteral("search: namespaced model keeps its owner"));
    check(models.value(2).description.contains(QLatin1String("NullGuard")),
          QStringLiteral("search: namespaced description"));
    check(models.value(2).capabilities == QStringList({QStringLiteral("tools"),
                                                       QStringLiteral("thinking")}),
          QStringLiteral("search: namespaced capabilities"));
    check(models.value(2).pulls == QLatin1String("614"),
          QStringLiteral("search: namespaced download count"));

    // Duplicate links must not duplicate rows.
    const auto deduped = RegistryClient::parseSearch(QStringLiteral(R"HTML(
<a href="/library/gemma4" class="group w-full">x</a>
<a href="/library/gemma4" class="group w-full">y</a>
)HTML"));
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
<div>
  <a href="/h4rithd/nullguard:latest" class="hidden md:grid">
    <span>h4rithd/nullguard:latest</span><span>9.3GB</span><span>8K</span><span>Text</span>
  </a>
</div>
)HTML");

    const auto tags = RegistryClient::parseTags(tagsHtml);
    check(tags.size() == 4, QStringLiteral("tags: duplicates collapsed"));
    check(tags.value(0).name == QLatin1String("qwen3:latest"), QStringLiteral("tags: name"));
    check(tags.value(0).size == QLatin1String("5.2GB"), QStringLiteral("tags: size"));
    check(tags.value(0).context == QLatin1String("40K"), QStringLiteral("tags: context"));
    check(tags.value(0).input == QLatin1String("Text"), QStringLiteral("tags: input"));
    check(tags.value(1).context == QLatin1String("256K"), QStringLiteral("tags: context 256K"));
    check(tags.value(2).input == QLatin1String("Vision"), QStringLiteral("tags: vision input"));
    check(tags.value(3).name == QLatin1String("h4rithd/nullguard:latest"),
          QStringLiteral("tags: namespaced name keeps its owner"));
    check(tags.value(3).size == QLatin1String("9.3GB"), QStringLiteral("tags: namespaced size"));
    check(tags.value(3).context == QLatin1String("8K"), QStringLiteral("tags: namespaced context"));

    // ------------------------------------------------ family page → details
    // Trimmed copy of ollama.com/library/qwen3: the og: meta, the header
    // "Updated … ago" pair, the capability chips and the hidden markdown
    // textarea (whose closing tag carries a newline before '>'), plus the
    // rendered readme used as the fallback.
    const QString modelHtml = QStringLiteral(R"HTML(
<head>
  <meta property="og:title" content="qwen3" />
  <meta property="og:description" content="Qwen3 is the latest generation of large language models in Qwen series, offering a comprehensive suite of dense and mixture-of-experts (MoE) models." />
</head>
<body>
  <p class="flex items-center flex-none">
    <span class="flex items-center space-x-1.5 text-xs">
      <span class="hidden sm:flex">Updated&nbsp;</span>
      <span >1 year ago</span>
    </span>
  </p>
  <div class="mb-0 flex flex-wrap gap-2">
    <span class="inline-flex items-center rounded-md bg-indigo-50 px-2 py-0.5 text-xs font-medium text-indigo-600 sm:text-[13px]">tools</span>
    <span class="inline-flex items-center rounded-md bg-purple-50 px-2 py-0.5 text-xs font-medium text-purple-600 sm:text-[13px]">thinking</span>
  </div>
  <span>30b</span><span>235b</span>
  <h2 class="text-base font-semibold leading-6 text-neutral-900">Readme</h2>
  <div
      id="display"
      class="prose prose-headings:font-semibold">
    <p><strong>Qwen 3</strong> is the latest generation of large language models in Qwen series.</p>
    <h3>New 30B model</h3>
  </div>
  <div id="editorContainer" class="hidden pt-4">
    <textarea
        id="editor"
        name="markdown"
        placeholder="Add some details"
        class="block max-h-[30em] min-h-[5rem] w-full"
    >![Qwen 3 logo](/assets/library/qwen3/a5541098-87ba-4184-a5af-2b63312c2522)

**Qwen 3** is the latest generation of large language models in Qwen series.

### New 30B model

```
ollama run qwen3:30b
```

- **Support of 100&#43; languages and dialects**</textarea
    >
  </div>
</body>
)HTML");

    const auto modelDetails = RegistryClient::parseModelPage(modelHtml);
    check(modelDetails.description.contains(QLatin1String("mixture-of-experts")),
          QStringLiteral("model page: og:description"));
    check(modelDetails.description.endsWith(QLatin1String("(MoE) models.")),
          QStringLiteral("model page: description not truncated by a quote"));
    check(modelDetails.readmeMarkdown.contains(QLatin1String("**Qwen 3** is the latest")),
          QStringLiteral("model page: readme markdown"));
    check(modelDetails.readmeMarkdown.contains(QLatin1String("ollama run qwen3:30b")),
          QStringLiteral("model page: readme keeps code blocks and newlines"));
    check(modelDetails.readmeMarkdown.contains(QLatin1String("100+ languages")),
          QStringLiteral("model page: numeric entity decoded in the readme"));
    check(modelDetails.readmeMarkdown.startsWith(QLatin1String("![Qwen 3 logo]")),
          QStringLiteral("model page: textarea markdown wins over div#display"));
    check(modelDetails.updated == QLatin1String("1 year ago"),
          QStringLiteral("model page: updated age"));
    check(modelDetails.capabilities.contains(QLatin1String("tools"))
              && modelDetails.capabilities.contains(QLatin1String("thinking")),
          QStringLiteral("model page: capabilities"));
    check(modelDetails.sizes == QStringList({QStringLiteral("30b"), QStringLiteral("235b")}),
          QStringLiteral("model page: sizes"));

    // Pages without a textarea still get a description from the rendered copy.
    const QString legacyHtml = QStringLiteral(R"HTML(
<meta property="og:description" content="A small model." />
<div
    id="display"
    class="prose">
  <p>Rendered <strong>readme</strong> text.</p>
</div>
<p class="hidden sm:block">Updated 2 months ago</p>
)HTML");
    const auto legacyDetails = RegistryClient::parseModelPage(legacyHtml);
    check(legacyDetails.readmeMarkdown == QLatin1String("Rendered readme text."),
          QStringLiteral("model page: div#display fallback as readme"));
    check(legacyDetails.updated == QLatin1String("2 months ago"),
          QStringLiteral("model page: updated from the details header"));

    // ------------------------------------------------ tag page → tag details
    // Trimmed blob rows of ollama.com/library/qwen3:30b-a3b-instruct-2507-q4_K_M.
    const QString tagHtml = QStringLiteral(R"HTML(
<div class="group block grid-cols-12 gap-2 px-4 py-3 sm:grid sm:grid-cols-12">
  <div class="truncate text-sm font-medium text-neutral-800 group-hover:underline sm:col-span-2 sm:col-start-1">
    <a href="/library/qwen3:30b-a3b-instruct-2507-q4_K_M/blobs/78b329e716e7">
      model
    </a>
  </div>
  <div class="truncate font-mono text-[13px] text-neutral-400 subpixel-antialiased sm:col-span-8 sm:col-start-3">
    <div class="space-x-2 flex text-sm">
      <div class="flex sm:space-x-2 items-center"><span class="hidden sm:block">arch</span><span class="text-neutral-400 sm:font-semibold sm:text-neutral-800 sm:text-xs">qwen3moe</span></div>
      <div>·</div>
      <div class="flex sm:space-x-2 items-center"><span class="hidden sm:block">parameters</span><span class="text-neutral-400 sm:font-semibold sm:text-neutral-800 sm:text-xs">30.5B</span></div>
      <div>·</div>
      <div class="flex sm:space-x-2 items-center"><span class="hidden sm:block">quantization</span><span class="text-neutral-400 sm:font-semibold sm:text-neutral-800 sm:text-xs">Q4_K_M</span></div>
    </div>
  </div>
  <div class="hidden text-right text-xs text-neutral-400 sm:col-start-12 sm:block">
    19GB
  </div>
</div>
<div class="group block grid-cols-12 gap-2 px-4 py-3 sm:grid sm:grid-cols-12">
  <div class="truncate text-sm font-medium text-neutral-800 group-hover:underline sm:col-span-2 sm:col-start-1">
    <a href="/library/qwen3:30b-a3b-instruct-2507-q4_K_M/blobs/d18a5cc71b84">
      license
    </a>
  </div>
  <div class="truncate font-mono text-[13px] text-neutral-400 subpixel-antialiased sm:col-span-8 sm:col-start-3">
    Apache License
Version 2.0, January 2004
http://www.apache.org/licenses/
TERMS AND CONDITIONS FOR US
  </div>
  <div class="hidden text-right text-xs text-neutral-400 sm:col-start-12 sm:block">
    11kB
  </div>
</div>
)HTML");

    const auto tagDetails = RegistryClient::parseTagPage(tagHtml);
    check(tagDetails.architecture == QLatin1String("qwen3moe"),
          QStringLiteral("tag page: architecture"));
    check(tagDetails.parameters == QLatin1String("30.5B"),
          QStringLiteral("tag page: parameters"));
    check(tagDetails.quantization == QLatin1String("Q4_K_M"),
          QStringLiteral("tag page: quantization"));
    check(tagDetails.license == QLatin1String("Apache License"),
          QStringLiteral("tag page: license first line"));

    check(RegistryClient::parseSearch(QString()).isEmpty(), QStringLiteral("search: empty page"));
    check(RegistryClient::parseTags(QString()).isEmpty(), QStringLiteral("tags: empty page"));

    // A page with nothing recognisable must produce empty fields, not a crash.
    const auto emptyModel = RegistryClient::parseModelPage(QString());
    check(emptyModel.description.isEmpty() && emptyModel.readmeMarkdown.isEmpty()
              && emptyModel.updated.isEmpty() && emptyModel.capabilities.isEmpty()
              && emptyModel.sizes.isEmpty(),
          QStringLiteral("model page: empty page → empty fields"));
    const auto junkModel = RegistryClient::parseModelPage(QStringLiteral("<html><body/></html>"));
    check(junkModel.description.isEmpty() && junkModel.readmeMarkdown.isEmpty()
              && junkModel.updated.isEmpty(),
          QStringLiteral("model page: markup change → empty fields"));
    const auto emptyTag = RegistryClient::parseTagPage(QString());
    check(emptyTag.architecture.isEmpty() && emptyTag.parameters.isEmpty()
              && emptyTag.quantization.isEmpty() && emptyTag.license.isEmpty(),
          QStringLiteral("tag page: empty page → empty fields"));
    const auto junkTag = RegistryClient::parseTagPage(QStringLiteral("<p>nope</p>"));
    check(junkTag.architecture.isEmpty() && junkTag.license.isEmpty(),
          QStringLiteral("tag page: markup change → empty fields"));

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
