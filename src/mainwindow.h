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

class TokenManager;
class ProxyService;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    /**
     * tokens/service принадлежат вызывающему (main): в приложении только один
     * TokenManager, иначе диалог токенов и прокси смотрели бы в разные кошельки.
     */
    explicit MainWindow(TokenManager *tokens, ProxyService *service,
                        QWidget *parent = nullptr);

    /** Прятать окно в трей вместо закрытия (когда трей доступен). */
    void setHideOnClose(bool hide) { m_hideOnClose = hide; }

signals:
    /** Окно скрылось в трей — main показывает уведомление (один раз). */
    void hiddenToTray();

public slots:
    void showTokensDialog();
    void showProxyDialog();

private slots:
    void onModelSelected(const QString &modelName);
    void onNewModel();
    void onDeleteModel(const QString &modelName);
    void onRenameModel(const QString &modelName);
    void onImportGGUF();
    void onCreateModel(const QString &name, const QString &modelfileContent);
    void onUpdateModel(const QString &name, const QString &modelfileContent);
    void onRefreshAll();
    void onManageTokens();

private:
    void setupUI();
    void setupMenuBar();
    void buildLanguageMenu();
    void retranslateUi();
    void checkOllamaAvailable();

    void closeEvent(QCloseEvent *event) override;

    QString writeTempModelfile(const QString &content);

    OllamaCli *m_cli;
    TokenManager *m_tokens;
    ProxyService *m_proxy;
    bool m_hideOnClose = false;
    ModelListWidget *m_listWidget;
    ModelEditorWidget *m_editorWidget;
    QSplitter *m_splitter;

    QMenu *m_fileMenu;
    QMenu *m_languageMenu;
    QMenu *m_toolsMenu;
    QMenu *m_helpMenu;
    QAction *m_newAction;
    QAction *m_importAction;
    QAction *m_refreshAction;
    QAction *m_quitAction;
    QAction *m_tokensAction;
    QAction *m_proxyAction;
    QAction *m_aboutAction;
    QActionGroup *m_langGroup;
};

#endif // MAINWINDOW_H
