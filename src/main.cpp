#include <QApplication>
#include <QStyle>
#include <QDir>
#include <QSettings>
#include "mainwindow.h"
#include "i18n.h"

static QString findLanguagesDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + "/languages",
        appDir + "/../languages",
        appDir + "/../share/ollama-model-manager/languages",
        "/usr/share/ollama-model-manager/languages",
        "/usr/local/share/ollama-model-manager/languages",
    };
    for (const QString &c : candidates) {
        if (QDir(c).exists())
            return QDir(c).absolutePath();
    }
    return appDir + "/languages";
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("Ollama Model Manager");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("OllamaManager");

    app.setStyleSheet(
        "QToolTip {"
        "  max-width: 420px;"
        "  padding: 6px 10px;"
        "  border: 1px solid #888;"
        "  border-radius: 4px;"
        "  background: #fffef2;"
        "  color: #222;"
        "  font-size: 12px;"
        "}"
    );

    I18n::instance()->setLanguagesDir(findLanguagesDir());
    QString saved = QSettings().value("language", "en").toString();
    if (!I18n::instance()->setLanguage(saved))
        I18n::instance()->setLanguage("en");

    MainWindow window;
    window.show();

    return app.exec();
}
