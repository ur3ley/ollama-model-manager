// Smoke test for the unified search/install dialog.
//
// Offline (default): constructs the dialog and checks the initial state.
//
// Live modes (need network / Ollama):
//   QT_QPA_PLATFORM=offscreen ./modelsearchdialog-selftest --live "qwen3" /tmp/shot.png
//   QT_QPA_PLATFORM=offscreen ./modelsearchdialog-selftest --pull qwen3:0.6b
//
// `--pull` runs a real `ollama pull` through OllamaCli and prints the parsed
// progress lines, so the progress regexes can be checked without the GUI.

#include "modelsearchdialog.h"
#include "ollamacli.h"
#include "i18n.h"

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

    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--pull")) {
        const QString name = QString::fromLocal8Bit(argv[2]);
        bool finished = false;
        bool ok = false;
        int progressLines = 0;

        QObject::connect(&cli, &OllamaCli::pullProgress, &app,
                         [&](const QString &status, int percent) {
            ++progressLines;
            if (progressLines <= 12 || percent == 100)
                QTextStream(stdout) << "  [" << percent << "%] " << status << "\n";
        });
        QObject::connect(&cli, &OllamaCli::pullFinished, &app,
                         [&](bool success, const QString &output) {
            finished = true;
            ok = success;
            if (!success)
                QTextStream(stdout) << "output: " << output << "\n";
        });

        cli.pullModelAsync(name);
        if (!waitFor([&] { return finished; }, 600000)) {
            QTextStream(stdout) << "FAIL pull timed out\n";
            return 1;
        }
        check(ok, QStringLiteral("pull succeeded"));
        check(progressLines > 0, QStringLiteral("progress lines parsed"));
        QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n" : "FAILURES\n");
        return g_failures == 0 ? 0 : 1;
    }

    if (argc > 3 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--live")) {
        const QString query = QString::fromLocal8Bit(argv[2]);
        const QString shot = QString::fromLocal8Bit(argv[3]);

        ModelSearchDialog dlg(&cli);
        dlg.resize(900, 760);
        dlg.show();
        dlg.searchAs(query);

        if (!waitFor([&] { return dlg.modelCount() > 0; }, 40000)) {
            QTextStream(stdout) << "FAIL no models; status: " << dlg.statusText() << "\n";
            return 1;
        }
        dlg.selectModel(0);
        if (!waitFor([&] { return dlg.tagCount() > 0; }, 40000)) {
            QTextStream(stdout) << "FAIL no tags; status: " << dlg.statusText() << "\n";
            return 1;
        }
        dlg.selectTag(0);

        QTextStream(stdout) << "models: " << dlg.modelCount()
                            << ", tags: " << dlg.tagCount()
                            << ", suggested: " << dlg.suggestedName() << "\n";
        QCoreApplication::processEvents();
        const bool saved = dlg.grab().save(shot);
        QTextStream(stdout) << (saved ? "saved\n" : "save failed\n");

        dlg.showHfTab();
        if (!waitFor([&] { return dlg.hfModelCount() > 0; }, 40000)) {
            QTextStream(stdout) << "FAIL no HF repos; status: " << dlg.statusText() << "\n";
            return 1;
        }
        dlg.selectHfModel(0);
        waitFor([&] { return dlg.hfVariantCount() > 0; }, 30000);
        QCoreApplication::processEvents();
        const QString hfShot = shot + QStringLiteral(".hf.png");
        const bool savedHf = dlg.grab().save(hfShot);
        QTextStream(stdout) << "hf repos: " << dlg.hfModelCount() << "\n"
                            << (savedHf ? "saved hf\n" : "save hf failed\n");

        dlg.showDeviceTab();
        if (!waitFor([&] { return dlg.deviceCount() > 0; }, 30000)) {
            QTextStream(stdout) << "no installed models yet\n";
            return (saved && savedHf) ? 0 : 1;
        }
        dlg.selectDevice(0);
        waitFor([&] { return dlg.deviceVariantCount() > 1; }, 30000);
        QCoreApplication::processEvents();
        const QString devShot = shot + QStringLiteral(".device.png");
        const bool savedDev = dlg.grab().save(devShot);
        QTextStream(stdout) << "installed: " << dlg.deviceCount() << "\n"
                            << (savedDev ? "saved device\n" : "save device failed\n");
        return (saved && savedHf && savedDev) ? 0 : 1;
    }

    {
        ModelSearchDialog dlg(&cli);
        check(dlg.modelCount() == 0, QStringLiteral("no models on start"));
        check(dlg.tagCount() == 0, QStringLiteral("no tags on start"));
        check(dlg.suggestedName().isEmpty(), QStringLiteral("empty name field"));
    }

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
