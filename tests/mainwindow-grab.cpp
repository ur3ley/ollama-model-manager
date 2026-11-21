// Dev aid: render the main window offscreen and save a PNG.
//
//   QT_QPA_PLATFORM=offscreen ./mainwindow-grab /tmp/main.png [tabIndex]
//
// Handy for checking the layout without opening a real window. Uses the real
// Ollama instance for the model list; no wallet data is touched.

#include "mainwindow.h"
#include "tokenmanager.h"
#include "i18n.h"

#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QListWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QThread>

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    auto *i18n = I18n::instance();
    i18n->setLanguagesDir(QCoreApplication::applicationDirPath() + "/languages");
    i18n->setLanguage(argc > 2 && QString::fromLocal8Bit(argv[2]) == QLatin1String("en")
                              ? QStringLiteral("en") : QStringLiteral("ru"));

    const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("/tmp/mainwindow.png");
    const int tab = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 0;
    const int modelRow = argc > 4 ? QString::fromLocal8Bit(argv[4]).toInt() : -1;

    TokenManager tokens;
    MainWindow window(&tokens, nullptr);
    window.resize(1180, 780);
    window.show();

    // Let the model list and the panel load.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2500) {
        QCoreApplication::processEvents();
        QThread::msleep(20);
    }

    if (modelRow >= 0) {
        if (auto *list = window.findChild<QListWidget *>(QStringLiteral("modelList"))) {
            if (modelRow < list->count())
                list->setCurrentRow(modelRow);
        }
        timer.restart();
        while (timer.elapsed() < 1200) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
    }

    if (auto *tabs = window.findChild<QTabWidget *>()) {
        if (tab >= 0 && tab < tabs->count())
            tabs->setCurrentIndex(tab);
        timer.restart();
        while (timer.elapsed() < 1500) {
            QCoreApplication::processEvents();
            QThread::msleep(20);
        }
    }

    const bool saved = window.grab().save(path);
    QTextStream(stdout) << (saved ? "saved\n" : "save failed\n");
    return saved ? 0 : 1;
}
