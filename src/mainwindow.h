#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSplitter>
#include <QTemporaryFile>
#include "ollamacli.h"
#include "modellistwidget.h"
#include "modeleditorwidget.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onModelSelected(const QString &modelName);
    void onNewModel();
    void onDeleteModel(const QString &modelName);
    void onRenameModel(const QString &modelName);
    void onImportGGUF();
    void onCreateModel(const QString &name, const QString &modelfileContent);
    void onUpdateModel(const QString &name, const QString &modelfileContent);
    void onRefreshAll();

private:
    void setupUI();
    void setupMenuBar();
    void checkOllamaAvailable();

    QString writeTempModelfile(const QString &content);

    OllamaCli *m_cli;
    ModelListWidget *m_listWidget;
    ModelEditorWidget *m_editorWidget;
    QSplitter *m_splitter;
};

#endif // MAINWINDOW_H
