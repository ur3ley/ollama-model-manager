// Offline self-test for the provenance store (models.json).
//
// Uses XDG_CONFIG_HOME pointing at a temp dir, so the real config is untouched.
//
//   ./provenance-selftest     → PASS/FAIL lines, exit 0/1

#include "provenance.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    Provenance &store = Provenance::instance();
    check(store.lookup(QStringLiteral("nothing:here")).isEmpty(),
          QStringLiteral("unknown model has no provenance"));

    Provenance::Entry ollama;
    ollama.source = QStringLiteral("ollama");
    ollama.ref = QStringLiteral("qwen3:8b");
    store.record(QStringLiteral("qwen3:8b"), ollama);

    Provenance::Entry hf;
    hf.source = QStringLiteral("hf");
    hf.ref = QStringLiteral("unsloth/Qwen3-30B-A3B-Instruct-2507-GGUF");
    hf.file = QStringLiteral("Qwen3-30B-A3B-Instruct-2507-Q4_K_M.gguf");
    hf.sha = QStringLiteral("deadbeef");
    store.record(QStringLiteral("qwen3-30b-a3b-instruct-2507:q4_k_m"), hf);

    check(store.contains(QStringLiteral("qwen3:8b")), QStringLiteral("ollama entry stored"));
    check(store.lookup(QStringLiteral("qwen3:8b")).ref == QLatin1String("qwen3:8b"),
          QStringLiteral("ollama ref round-trip"));
    check(!store.lookup(QStringLiteral("qwen3:8b")).installedAt.isEmpty(),
          QStringLiteral("install time recorded"));

    const Provenance::Entry hfEntry = store.lookup(QStringLiteral("qwen3-30b-a3b-instruct-2507:q4_k_m"));
    check(hfEntry.source == QLatin1String("hf"), QStringLiteral("hf source"));
    check(hfEntry.file.endsWith(QLatin1String("Q4_K_M.gguf")), QStringLiteral("hf file"));
    check(hfEntry.sha == QLatin1String("deadbeef"), QStringLiteral("hf sha"));

    store.remove(QStringLiteral("qwen3:8b"));
    check(!store.contains(QStringLiteral("qwen3:8b")), QStringLiteral("entry removed"));

    // A-4: one-time move of models.json from the old AppConfigLocation.
    {
        QTemporaryDir tmp;
        const QString from = tmp.filePath(QStringLiteral("old/models.json"));
        const QString to = tmp.filePath(QStringLiteral("new/models.json"));
        QDir().mkpath(tmp.filePath(QStringLiteral("old")));
        QFile legacy(from);
        check(legacy.open(QIODevice::WriteOnly), QStringLiteral("migrate: fixture written"));
        legacy.write("{\"m:1\":{\"source\":\"ollama\",\"ref\":\"m:1\"}}");
        legacy.close();

        check(Provenance::migrateLegacy(from, to), QStringLiteral("migrate: legacy file moved"));
        check(!QFile::exists(from) && QFile::exists(to),
              QStringLiteral("migrate: source gone, target present"));
        check((QFile::permissions(to) & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0,
              QStringLiteral("migrate: target is owner-only"));

        QFile again(from);
        check(again.open(QIODevice::WriteOnly), QStringLiteral("migrate: second fixture written"));
        again.write("{}");
        again.close();
        check(!Provenance::migrateLegacy(from, to),
              QStringLiteral("migrate: existing target is never overwritten"));
        QFile target(to);
        check(target.open(QIODevice::ReadOnly) && target.readAll().contains("m:1"), QStringLiteral("migrate: target content kept"));
        check(!Provenance::migrateLegacy(to, to), QStringLiteral("migrate: same path is a no-op"));
    }
    check(Provenance::storePath().endsWith(QLatin1String("/ollama-manager/models.json")),
          QStringLiteral("store lives in ~/.config/ollama-manager"));

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
