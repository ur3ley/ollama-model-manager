// Smoke test for the hub source panels (Ollama and HuggingFace).
//
// Offline (default): constructs both panels and checks the initial state.
//
// Live modes (need network / Ollama):
//   QT_QPA_PLATFORM=offscreen ./discoverpanel-selftest --live "qwen3" /tmp/shot.png
//   QT_QPA_PLATFORM=offscreen ./discoverpanel-selftest --pull qwen3:0.6b

#include "discoverpanel.h"
#include "provenance.h"
#include "installservice.h"
#include "i18n.h"
#include "ollamacli.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTextStream>
#include <QThread>

#include <functional>

namespace {

int g_failures = 0;

void check(bool condition, const QString &what)
{
    QTextStream out(stdout);
    out << (condition ? "PASS " : "FAIL ") << what << "\n";
    if (!condition)
        ++g_failures;
}

bool waitFor(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(10);
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    auto *i18n = I18n::instance();
    i18n->setLanguagesDir(QCoreApplication::applicationDirPath() + "/languages");
    i18n->setLanguage(QStringLiteral("en"));

    OllamaCli cli;
    InstallService install(&cli, &Provenance::instance());

    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--pull")) {
        const QString name = QString::fromLocal8Bit(argv[2]);
        // Optional third argument: the size the registry advertises, in bytes.
        const qint64 expected = argc > 3 ? QString::fromLocal8Bit(argv[3]).toLongLong() : 0;
        bool finished = false;
        bool ok = false;
        int progressLines = 0;
        int lastPercent = -2;
        bool monotonic = true;
        OllamaJob *job = cli.pullModelAsync(name, expected);
        QObject::connect(job, &OllamaJob::progress, &app,
                         [&](const QString &label, int percent, qint64 received, qint64 total) {
            ++progressLines;
            if (percent >= 0 && lastPercent >= 0 && percent < lastPercent)
                monotonic = false;
            lastPercent = percent;
            if (progressLines <= 8 || percent == 100)
                QTextStream(stdout) << "  [" << percent << "%] " << (received >> 20)
                                    << " MB / " << (total >> 20) << " MB (" << label << ")\n";
        });
        QObject::connect(job, &OllamaJob::finished, &app,
                         [&](bool success, const QString &output) {
            finished = true;
            ok = success;
            if (!success)
                QTextStream(stdout) << "output: " << output << "\n";
        });
        if (!waitFor([&] { return finished; }, 600000)) {
            QTextStream(stdout) << "FAIL pull timed out\n";
            return 1;
        }
        check(ok, QStringLiteral("pull succeeded"));
        check(progressLines > 0, QStringLiteral("progress lines parsed"));
        check(monotonic, QStringLiteral("progress never goes backwards"));
        check(lastPercent == 100, QStringLiteral("progress ends at 100%"));
        QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n" : "FAILURES\n");
        return g_failures == 0 ? 0 : 1;
    }

    if (argc > 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--live")) {
        const QString query = QString::fromLocal8Bit(argv[2]);
        const QString shot = QString::fromLocal8Bit(argv[3]);

        DiscoverPanel ollama(DiscoverPanel::Source::Ollama, &install);
        ollama.resize(1000, 700);
        ollama.show();
        ollama.searchAs(query);
        if (!waitFor([&] { return ollama.modelCount() > 0; }, 40000)) {
            QTextStream(stdout) << "FAIL no models; status: " << ollama.statusText() << "\n";
            return 1;
        }
        ollama.selectModel(0);
        if (!waitFor([&] { return ollama.tagCount() > 0; }, 40000)) {
            QTextStream(stdout) << "FAIL no tags; status: " << ollama.statusText() << "\n";
            return 1;
        }
        ollama.selectTag(0);
        QCoreApplication::processEvents();
        const bool savedOllama = ollama.grab().save(shot);
        QTextStream(stdout) << "ollama models: " << ollama.modelCount()
                            << ", variants: " << ollama.tagCount()
                            << ", suggested: " << ollama.suggestedName() << "\n"
                            << (savedOllama ? "saved ollama\n" : "save ollama failed\n");

        DiscoverPanel hf(DiscoverPanel::Source::HuggingFace, &install);
        hf.resize(1000, 700);
        hf.show();
        hf.searchAs(QString());
        if (!waitFor([&] { return hf.modelCount() > 0; }, 40000)) {
            QTextStream(stdout) << "FAIL no HF repos; status: " << hf.statusText() << "\n";
            return 1;
        }
        hf.selectModel(0);
        waitFor([&] { return hf.tagCount() > 0; }, 30000);
        QCoreApplication::processEvents();
        const QString hfShot = shot + QStringLiteral(".hf.png");
        const bool savedHf = hf.grab().save(hfShot);
        QTextStream(stdout) << "hf repos: " << hf.modelCount()
                            << ", files: " << hf.tagCount() << "\n"
                            << (savedHf ? "saved hf\n" : "save hf failed\n");
        return (savedOllama && savedHf) ? 0 : 1;
    }

    if (argc > 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--queue")) {
        // Live check of the download queue: start a pull, then start another
        // one from the same panel and require it to be queued, not ignored.
        const QString firstQuery = QString::fromLocal8Bit(argv[2]);
        const QString secondQuery = QString::fromLocal8Bit(argv[3]);

        int sigActive = -1;
        int sigQueued = -1;
        QObject::connect(&cli, &OllamaCli::pullActivityChanged, &app,
                         [&](int active, int queued) {
            sigActive = active;
            sigQueued = queued;
        });

        DiscoverPanel panel(DiscoverPanel::Source::Ollama, &install);
        panel.resize(1000, 700);
        panel.show();

        auto prepare = [&](const QString &query) {
            panel.searchAs(query);
            if (!waitFor([&] { return panel.modelCount() > 0; }, 40000))
                return false;
            panel.selectModel(0);
            if (!waitFor([&] { return panel.tagCount() > 0; }, 40000))
                return false;
            panel.selectTag(0);
            QCoreApplication::processEvents();
            return true;
        };

        if (!prepare(firstQuery)) {
            QTextStream(stdout) << "FAIL first search: " << panel.statusText() << "\n";
            return 1;
        }
        panel.startInstall();
        check(waitFor([&] { return panel.statusText().contains("pull=running"); }, 30000),
              QStringLiteral("first pull is running"));

        if (!prepare(secondQuery)) {
            QTextStream(stdout) << "FAIL second search: " << panel.statusText() << "\n";
            QMetaObject::invokeMethod(&panel, "onCancel", Qt::DirectConnection);
            return 1;
        }
        panel.startInstall();
        const bool queued = waitFor([&] {
            return panel.statusText().contains("pull=queued:1");
        }, 15000);
        check(queued, QStringLiteral("second pull is queued, not swallowed"));
        check(sigActive == 1 && sigQueued == 1,
              QStringLiteral("activity signal reports 1 running + 1 queued"));
        check(panel.selectedVariantText().contains(QLatin1String("queued")),
              QStringLiteral("variant row shows the queued badge"));
        const QList<OllamaCli::PullInfo> list = cli.pullList();
        check(list.size() == 2 && list.at(0).active && !list.at(1).active,
              QStringLiteral("pull list: first runs, second waits"));
        QTextStream(stdout) << "status: " << panel.statusText() << "\n"
                            << "variant: " << panel.selectedVariantText() << "\n";

        // Promote the queued pull: the running one is suspended and requeued
        // right behind it, and the promoted row starts downloading.
        const quint64 queuedId = cli.pullList().at(1).id;
        check(cli.prioritizePull(queuedId), QStringLiteral("prioritize a queued pull"));
        check(waitFor([&] {
                  const auto current = cli.pullList();
                  return current.size() == 2 && current.at(0).id == queuedId
                          && current.at(0).active;
              }, 30000),
              QStringLiteral("promoted pull runs, the previous one waits behind it"));
        check(waitFor([&] {
                  return panel.selectedVariantText().contains(QLatin1String("downloading"));
              }, 20000),
              QStringLiteral("variant badge switches to downloading"));

        // Cancel the selected (now running) variant: only it stops, the
        // suspended one is promoted by the CLI.
        panel.cancelSelectedVariant();
        check(waitFor([&] {
                  return !panel.statusText().contains("pull=queued")
                          && panel.statusText().contains("pull=running");
              }, 20000),
              QStringLiteral("cancel variant stops only its own pull"));
        check(!panel.selectedVariantText().contains(QLatin1String("queued")),
              QStringLiteral("variant row loses the queued badge"));
        const QList<OllamaCli::PullInfo> left = cli.pullList();
        check(left.size() == 1 && left.at(0).active,
              QStringLiteral("only the running pull is left"));
        check(cli.cancelPull(left.at(0).id), QStringLiteral("cancel by id"));
        check(waitFor([&] { return cli.pullList().isEmpty(); }, 15000),
              QStringLiteral("pull list is empty"));

        QMetaObject::invokeMethod(&panel, "onCancel", Qt::DirectConnection);
        check(waitFor([&] { return !panel.statusText().contains("pull="); }, 20000),
              QStringLiteral("cancel cleared running and queued pulls"));
        QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n" : "FAILURES\n");
        return g_failures == 0 ? 0 : 1;
    }

    {
        DiscoverPanel ollama(DiscoverPanel::Source::Ollama, &install);
        DiscoverPanel hf(DiscoverPanel::Source::HuggingFace, &install);
        check(ollama.modelCount() == 0, QStringLiteral("ollama panel starts empty"));
        check(hf.modelCount() == 0, QStringLiteral("hf panel starts empty"));
        check(ollama.suggestedName().isEmpty(), QStringLiteral("nothing selected"));
    }

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
