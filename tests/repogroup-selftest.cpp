// Offline self-test for the installed-model grouping (repository cards).
//
// Uses XDG_CONFIG_HOME pointing at a temp dir, so the real models.json is
// untouched. No Ollama and no network are needed: the model list is synthetic.
//
//   ./repogroup-selftest     → PASS/FAIL lines, exit 0/1

#include "provenance.h"
#include "quantmatch.h"
#include "repogroup.h"

#include <QCoreApplication>
#include <QTemporaryDir>
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

const RepoGroup *findGroup(const QList<RepoGroup> &groups, const QString &id)
{
    for (const RepoGroup &group : groups) {
        if (group.id == id)
            return &group;
    }
    return nullptr;
}

OllamaModelInfo model(const QString &name, const QString &id,
                           const QString &size, const QString &modified)
{
    OllamaModelInfo info;
    info.name = name;
    info.id = id;
    info.size = size;
    info.modified = modified;
    return info;
}

} // namespace

int main(int argc, char *argv[])
{
    // Must happen before Provenance::instance() resolves storePath().
    QTemporaryDir configDir;
    if (!configDir.isValid()) {
        QTextStream(stdout) << "FAIL cannot create temp config dir\n";
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", configDir.path().toLocal8Bit());

    QCoreApplication app(argc, argv);

    Provenance &store = Provenance::instance();

    // ollama: two installed tags of one family.
    Provenance::Entry entry;
    entry.source = QStringLiteral("ollama");
    entry.ref = QStringLiteral("qwen3.8:latest");
    entry.digest = QStringLiteral("aaa111");
    store.record(QStringLiteral("qwen3.8:latest"), entry);
    entry.ref = QStringLiteral("qwen3.8:q4_K_M");
    store.record(QStringLiteral("qwen3.8:q4_K_M"), entry);

    // hf: two GGUF files of one repository.
    entry = Provenance::Entry();
    entry.source = QStringLiteral("hf");
    entry.ref = QStringLiteral("unsloth/Qwen3.8-27B-GGUF");
    entry.file = QStringLiteral("Qwen3.8-27B-Q4_K_M.gguf");
    entry.sha = QStringLiteral("beef01");
    store.record(QStringLiteral("qwen38:q4_k_m"), entry);
    entry.file = QStringLiteral("Qwen3.8-27B-Q8_0.gguf");
    store.record(QStringLiteral("qwen38:q8_0"), entry);

    // Renamed single-quant repository: the current name is not the ref.
    entry = Provenance::Entry();
    entry.source = QStringLiteral("ollama");
    entry.ref = QStringLiteral("translategemma:27b");
    store.record(QStringLiteral("translategemma:27b-it-q8_0"), entry);

    QList<OllamaModelInfo> models;
    models << model(QStringLiteral("qwen3.8:latest"), QStringLiteral("id1"),
                    QStringLiteral("17 GB"), QStringLiteral("2 days ago"))
           << model(QStringLiteral("qwen3.8:q4_K_M"), QStringLiteral("id2"),
                    QStringLiteral("10 GB"), QStringLiteral("3 days ago"))
           << model(QStringLiteral("qwen38:q4_k_m"), QStringLiteral("id3"),
                    QStringLiteral("8 GB"), QStringLiteral("1 day ago"))
           << model(QStringLiteral("qwen38:q8_0"), QStringLiteral("id4"),
                    QStringLiteral("9 GB"), QStringLiteral("2 days ago"))
           << model(QStringLiteral("translategemma:27b-it-q8_0"), QStringLiteral("id5"),
                    QStringLiteral("29 GB"), QStringLiteral("7 days ago"))
           << model(QStringLiteral("gemma4:e4b"), QStringLiteral("id6"),
                    QStringLiteral("6.6 GB"), QStringLiteral("7 days ago"));

    const QList<RepoGroup> groups = groupInstalledModels(models, store);

    check(groups.size() == 4, QStringLiteral("four groups (ollama, hf, renamed, local)"));
    check(groups.size() == 4 && groups.at(0).title == QLatin1String("gemma4:e4b"),
          QStringLiteral("groups sorted by title (case-insensitive)"));
    if (groups.size() == 4) {
        check(groups.at(1).title == QLatin1String("qwen3.8"),
              QStringLiteral("short family title sorts before its repo title"));
        check(groups.at(2).title == QLatin1String("Qwen3.8-27B-GGUF"),
              QStringLiteral("hf title is the part after the first slash"));
    }

    const RepoGroup *ollama = findGroup(groups, QStringLiteral("ollama:qwen3.8"));
    check(ollama != nullptr, QStringLiteral("ollama group id"));
    if (ollama) {
        check(ollama->source == QLatin1String("ollama"), QStringLiteral("ollama source"));
        check(ollama->owner == QLatin1String("ollama.com"), QStringLiteral("ollama owner"));
        check(!ollama->local, QStringLiteral("ollama group is not local"));
        check(ollama->quants.size() == 2, QStringLiteral("two installed quants in one repo"));
        check(ollama->quants.size() == 2
                      && ollama->quants.at(0).modelName == QLatin1String("qwen3.8:latest")
                      && ollama->quants.at(1).modelName == QLatin1String("qwen3.8:q4_K_M"),
              QStringLiteral("quants sorted by original ref"));
        check(ollama->originalRefOf(QStringLiteral("qwen3.8:q4_K_M"))
                      == QLatin1String("qwen3.8:q4_K_M"),
              QStringLiteral("originalRefOf returns the ref"));
        check(!ollama->quants.at(0).renamed(), QStringLiteral("untouched quant is not renamed"));
        check(ollama->searchText().contains(QStringLiteral("qwen3.8")),
              QStringLiteral("searchText covers the repository"));
    }

    const RepoGroup *hf = findGroup(groups, QStringLiteral("hf:unsloth/Qwen3.8-27B-GGUF"));
    check(hf != nullptr, QStringLiteral("hf group id uses the whole repo ref"));
    if (hf) {
        check(hf->owner == QLatin1String("unsloth"), QStringLiteral("hf owner"));
        check(hf->title == QLatin1String("Qwen3.8-27B-GGUF"), QStringLiteral("hf title"));
        check(hf->quants.size() == 2, QStringLiteral("two files of one hf repo share a card"));
        check(hf->quants.size() == 2
                      && hf->quants.at(0).file.endsWith(QLatin1String("Q4_K_M.gguf")),
              QStringLiteral("hf file name kept per quant"));
        check(hf->searchText().contains(QStringLiteral("Q8_0")),
              QStringLiteral("search by file name works"));
    }

    const RepoGroup *renamed = findGroup(groups, QStringLiteral("ollama:translategemma"));
    check(renamed != nullptr, QStringLiteral("renamed quant stays in its repository"));
    if (renamed && renamed->quants.size() == 1) {
        check(renamed->quants.at(0).renamed(), QStringLiteral("quant reports renamed()"));
        check(renamed->originalRefOf(QStringLiteral("translategemma:27b-it-q8_0"))
                      == QLatin1String("translategemma:27b"),
              QStringLiteral("original ref is not lost after a rename"));
        check(renamed->searchText().contains(QStringLiteral("translategemma:27b-it-q8_0")),
              QStringLiteral("search by the current name of a renamed quant"));
    } else {
        check(false, QStringLiteral("renamed repository has exactly one quant"));
    }

    const RepoGroup *local = findGroup(groups, QStringLiteral("local:gemma4:e4b"));
    check(local != nullptr, QStringLiteral("model without provenance is a local group"));
    if (local) {
        check(local->local, QStringLiteral("local flag"));
        check(local->owner.isEmpty(), QStringLiteral("local group has no owner"));
        check(local->title == QLatin1String("gemma4:e4b"), QStringLiteral("local title is the name"));
        check(local->quants.size() == 1 && !local->quants.at(0).renamed(),
              QStringLiteral("local quant is never 'renamed'"));
    }

    // Provenance::rename must move the entry and keep every field.
    store.rename(QStringLiteral("qwen3.8:latest"), QStringLiteral("qwen3.8:renamed"));    check(!store.contains(QStringLiteral("qwen3.8:latest")),
          QStringLiteral("rename drops the old key"));
    const Provenance::Entry moved = store.lookup(QStringLiteral("qwen3.8:renamed"));
    check(moved.source == QLatin1String("ollama"), QStringLiteral("rename keeps the source"));
    check(moved.ref == QLatin1String("qwen3.8:latest"), QStringLiteral("rename keeps the ref"));
    check(moved.digest == QStringLiteral("aaa111"), QStringLiteral("rename keeps the digest"));

    // Regrouping after the rename keeps one card and marks the quant as renamed.
    QList<OllamaModelInfo> afterRename;
    afterRename << model(QStringLiteral("qwen3.8:renamed"), QStringLiteral("id1"),
                         QStringLiteral("17 GB"), QStringLiteral("2 days ago"))
                << model(QStringLiteral("qwen3.8:q4_K_M"), QStringLiteral("id2"),
                         QStringLiteral("10 GB"), QStringLiteral("3 days ago"));
    const QList<RepoGroup> regrouped = groupInstalledModels(afterRename, store);
    check(regrouped.size() == 1, QStringLiteral("rename does not split the repository card"));
    if (regrouped.size() == 1) {
        check(regrouped.at(0).id == QLatin1String("ollama:qwen3.8"),
              QStringLiteral("group id is derived from the ref, not the name"));
        check(regrouped.at(0).quants.size() == 2, QStringLiteral("both quants still grouped"));
        bool hasRenamed = false;
        for (const InstalledQuant &quant : regrouped.at(0).quants) {
            if (quant.modelName == QLatin1String("qwen3.8:renamed") && quant.renamed())
                hasRenamed = true;
        }
        check(hasRenamed, QStringLiteral("renamed quant marked inside the group"));
    }

    // Binding a source back (without reinstalling) turns a local card into a
    // repository card; the local id disappears.
    Provenance::Entry restored;
    restored.source = QStringLiteral("ollama");
    restored.ref = QStringLiteral("gemma4:e4b");
    restored.digest = QStringLiteral("id6");
    store.record(QStringLiteral("gemma4:e4b"), restored);

    const QList<RepoGroup> afterBind = groupInstalledModels(models, store);
    const RepoGroup *bound = findGroup(afterBind, QStringLiteral("ollama:gemma4"));
    check(bound != nullptr, QStringLiteral("record() turns a local card into an ollama repository"));
    check(findGroup(afterBind, QStringLiteral("local:gemma4:e4b")) == nullptr,
          QStringLiteral("the local card is gone after binding"));
    if (bound) {
        check(!bound->local, QStringLiteral("bound group is not local"));
        check(bound->quants.size() == 1
                      && bound->quants.first().modelName == QLatin1String("gemma4:e4b"),
              QStringLiteral("bound model stays in its card"));
        check(!bound->quants.first().renamed(),
              QStringLiteral("a binding with the same name is not a rename"));
    }

    // Renaming the bound model keeps the repository card (ref-based grouping).
    store.rename(QStringLiteral("gemma4:e4b"), QStringLiteral("gemma4:renamed"));
    QList<OllamaModelInfo> renamedList;
    renamedList << model(QStringLiteral("gemma4:renamed"), QStringLiteral("id6"),
                         QStringLiteral("6.6 GB"), QStringLiteral("7 days ago"));
    const QList<RepoGroup> afterBoundRename = groupInstalledModels(renamedList, store);
    check(afterBoundRename.size() == 1
                  && afterBoundRename.first().id == QLatin1String("ollama:gemma4"),
          QStringLiteral("renaming a bound model keeps the ollama card"));
    if (afterBoundRename.size() == 1) {
        check(afterBoundRename.first().quants.first().renamed(),
              QStringLiteral("bound model reports renamed() after the rename"));
    }

    // Same for HuggingFace: a local model bound to a repo joins that repo card.
    QList<OllamaModelInfo> mystery;
    mystery << model(QStringLiteral("Mystery-GGUF:q4_k_m"), QStringLiteral("id7"),
                     QStringLiteral("5 GB"), QStringLiteral("1 day ago"));
    const QList<RepoGroup> mysteryLocal = groupInstalledModels(mystery, store);
    check(findGroup(mysteryLocal, QStringLiteral("local:Mystery-GGUF:q4_k_m")) != nullptr,
          QStringLiteral("unknown model starts as a local card"));
    Provenance::Entry hfEntry;
    hfEntry.source = QStringLiteral("hf");
    hfEntry.ref = QStringLiteral("someone/Mystery-GGUF");
    hfEntry.file = QStringLiteral("Mystery-Q4_K_M.gguf");
    hfEntry.sha = QStringLiteral("cafe01");
    hfEntry.installedAt = QStringLiteral("2026-01-02T03:04:05Z");
    store.record(QStringLiteral("Mystery-GGUF:q4_k_m"), hfEntry);
    const QList<RepoGroup> mysteryBound = groupInstalledModels(mystery, store);
    const RepoGroup *hfBound = findGroup(mysteryBound, QStringLiteral("hf:someone/Mystery-GGUF"));
    check(hfBound != nullptr, QStringLiteral("record() turns a local card into an hf repository"));
    if (hfBound) {
        check(hfBound->owner == QLatin1String("someone"), QStringLiteral("hf bound owner"));
        check(hfBound->title == QLatin1String("Mystery-GGUF"), QStringLiteral("hf bound title"));
        check(hfBound->quants.first().file.endsWith(QLatin1String("Q4_K_M.gguf")),
              QStringLiteral("hf bound file is kept"));
        check(store.lookup(QStringLiteral("Mystery-GGUF:q4_k_m")).installedAt
                      == QLatin1String("2026-01-02T03:04:05Z"),
              QStringLiteral("record keeps an explicit installedAt"));
    }

    // digest comparison used by the automatic source search.
    check(digestMatches(QStringLiteral("aabbccdd11223344"),
                        QStringLiteral("sha256:aabbccdd11223344aabbccdd")),
          QStringLiteral("digestMatches: local id is a prefix of the registry digest"));
    check(digestMatches(QStringLiteral("sha256:AABBCCDD11223344"),
                        QStringLiteral("aabbccdd11223344aabbccdd")),
          QStringLiteral("digestMatches: sha256 prefix and case are ignored"));
    check(!digestMatches(QStringLiteral("aabbccdd11223344"),
                         QStringLiteral("sha256:ffffffffffffffffffff")),
          QStringLiteral("digestMatches: different digests do not match"));
    check(!digestMatches(QString(), QStringLiteral("sha256:aabbccdd11223344")),
          QStringLiteral("digestMatches: an empty local id never matches"));
    check(digestMatches(QStringLiteral("abc123"), QStringLiteral("abc123")),
          QStringLiteral("digestMatches: short equal values match"));
    check(!digestMatches(QStringLiteral("abc123"), QStringLiteral("abc124")),
          QStringLiteral("digestMatches: short different values do not match"));

    // Three states of a browse variant: the label mark and the action button
    // both derive from these two facts.
    check(variantState(false, false) == VariantState::NotInstalled,
          QStringLiteral("variantState: not installed → download"));
    check(variantState(false, true) == VariantState::NotInstalled,
          QStringLiteral("variantState: not installed ignores the source match"));
    check(variantState(true, true) == VariantState::Installed,
          QStringLiteral("variantState: installed and current → downloaded"));
    check(variantState(true, false) == VariantState::UpdateAvailable,
          QStringLiteral("variantState: installed and stale → update"));

    // HuggingFace commit comparison (update mark for installed GGUFs).
    const QString shaA = QStringLiteral("0123456789abcdef0123456789abcdef01234567");
    const QString shaB = QStringLiteral("89abcdef0123456789abcdef0123456789abcdef");
    check(!shaDiffers(shaA, shaA), QStringLiteral("shaDiffers: same commit → no update"));
    check(!shaDiffers(QStringLiteral("01234567"), shaA),
          QStringLiteral("shaDiffers: a short stored sha is a prefix of the same commit"));
    check(shaDiffers(shaA, shaB), QStringLiteral("shaDiffers: different commits → update"));
    check(!shaDiffers(QString(), shaA),
          QStringLiteral("shaDiffers: no stored sha → no update (entries from before)"));
    check(!shaDiffers(shaA, QString()),
          QStringLiteral("shaDiffers: no remote sha → no update"));

    // GGUF variant identity: the provenance file, not the generated model name,
    // decides whether a picker row is installed. A model created from the MTP
    // files under the old (suffix-less) name must mark only the MTP row.
    const QString mtpEntryFile =
            QStringLiteral("Qwen3.8-Flash-Next-Uncensored-IQ4_XS-MTP-00001-of-00003.gguf");
    const QString mtpVariantFile =
            QStringLiteral("Qwen3.8-Flash-Next-Uncensored-IQ4_XS-MTP-00001-of-00003.gguf");
    const QString plainVariantFile =
            QStringLiteral("Qwen3.8-Flash-Next-Uncensored-IQ4_XS.gguf");
    Provenance::Entry legacy;
    legacy.source = QStringLiteral("hf");
    legacy.ref = QStringLiteral("orcarouter/Qwen3.8-Flash-Next-Uncensored-GGUF");
    legacy.file = mtpEntryFile; // created by the old code under ":iq4_xs"
    store.record(QStringLiteral("qwen3.8-flash-next-uncensored:iq4_xs"), legacy);

    check(sameGgufVariant(mtpEntryFile, mtpVariantFile),
          QStringLiteral("sameGgufVariant: the installed MTP file owns the MTP row"));
    check(!sameGgufVariant(mtpEntryFile, plainVariantFile),
          QStringLiteral("sameGgufVariant: the MTP install does not mark the plain IQ4_XS row"));
    check(sameGgufVariant(QStringLiteral("Q4_K_M/model-Q4_K_M-00002-of-00003.gguf"),
                          QStringLiteral("Q4_K_M/model-Q4_K_M-00001-of-00003.gguf")),
          QStringLiteral("sameGgufVariant: split parts of one quant are one variant"));
    check(sameGgufVariant(QStringLiteral("Model-Q4_K_M.gguf"),
                          QStringLiteral("model-q4_k_m.gguf")),
          QStringLiteral("sameGgufVariant: case and path are ignored"));
    check(!sameGgufVariant(QStringLiteral("model-Q4_K_M.gguf"), QString()),
          QStringLiteral("sameGgufVariant: an empty file never matches"));
    check(ggufFileNameStem(QStringLiteral("Q4_K_M/model-Q4_K_M-00001-of-00003.gguf"))
                  == QLatin1String("model-Q4_K_M"),
          QStringLiteral("ggufFileNameStem drops path, part suffix and .gguf"));
    check(ggufFileNameStem(QStringLiteral("model.gguf")) == QLatin1String("model"),
          QStringLiteral("ggufFileNameStem drops .gguf"));

    // The stored entry still names the repository after a rename (the file and
    // ref are untouched by Provenance::rename), so the picker can find it.
    store.rename(QStringLiteral("qwen3.8-flash-next-uncensored:iq4_xs"),
                 QStringLiteral("renamed-mtp"));
    const Provenance::Entry renamedEntry = store.lookup(QStringLiteral("renamed-mtp"));
    check(renamedEntry.ref == QLatin1String("orcarouter/Qwen3.8-Flash-Next-Uncensored-GGUF")
                  && sameGgufVariant(renamedEntry.file, mtpVariantFile),
          QStringLiteral("provenance: a renamed model still matches its repository file"));

    // Quant-suffix stripping shared by the editor and the binding dialog.
    check(stripQuantSuffix(QStringLiteral("8b-instruct-q4_k_m"))
                  == QLatin1String("8b-instruct"),
          QStringLiteral("stripQuantSuffix removes -q4_k_m"));
    check(stripQuantSuffix(QStringLiteral("27b-it-q8_0")) == QLatin1String("27b-it"),
          QStringLiteral("stripQuantSuffix removes -q8_0"));
    check(stripQuantSuffix(QStringLiteral("latest")) == QLatin1String("latest"),
          QStringLiteral("stripQuantSuffix keeps a plain tag"));
    check(bindSearchStem(QStringLiteral("qwen3.8:latest")) == QLatin1String("qwen3.8"),
          QStringLiteral("bindSearchStem uses the family"));
    check(bindSearchStem(QStringLiteral("translategemma:27b-it-q8_0"))
                  == QLatin1String("translategemma"),
          QStringLiteral("bindSearchStem drops the quant tag"));

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
