#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSplitter>
#include <QTemporaryFile>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include "ollamacli.h"
#include "modellistwidget.h"
#include "modeleditorwidget.h"
#include "i18n.h"

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
    void buildLanguageMenu();
    void retranslateUi();
    void checkOllamaAvailable();

    QString writeTempModelfile(const QString &content);

    OllamaCli *m_cli;
    ModelListWidget *m_listWidget;
    ModelEditorWidget *m_editorWidget;
    QSplitter *m_splitter;

    QMenu *m_fileMenu;
    QMenu *m_languageMenu;
    QMenu *m_helpMenu;
    QAction *m_newAction;
    QAction *m_importAction;
    QAction *m_refreshAction;
    QAction *m_quitAction;
    QAction *m_aboutAction;
    QActionGroup *m_langGroup;
};

#endif // MAINWINDOW_H
