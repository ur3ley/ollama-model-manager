#include <QApplication>
#include <QStyle>
#include "mainwindow.h"

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

    MainWindow window;
    window.show();

    return app.exec();
}
