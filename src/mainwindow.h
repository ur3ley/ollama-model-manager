#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSplitter>
#include <QMenu>
#include <QAction>
#include <QActionGroup>
#include "ollamacli.h"
#include "modellistwidget.h"
#include "modeleditorwidget.h"
#include "i18n.h"

class InstallService;
class TokenManager;
class ProxyService;
class DiscoverPanel;
class StartPageHeader;
class QTabWidget;
class QTimer;

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
    void onRepoSelected(const QString &id);
    void onNewModel();
    void onDeleteModel(const QString &modelName);
    void onRenameModel(const QString &modelName);
    void onImportGGUF();
    void onDownloadModel();
    void onCreateModel(const QString &name, const QString &modelfileContent);
    void onUpdateModel(const QString &name, const QString &modelfileContent);
    void onRenameModelDirect(const QString &oldName, const QString &newName);
    void onRenameAndUpdateModel(const QString &oldName, const QString &newName,
                                const QString &modelfileContent);
    void onRefreshAll();
    void onManageTokens();
    /** Restore provenance for one installed model without reinstalling it. */
    void onBindSourceRequested(const QString &modelName);
    /** Batch pass over every local card ("Tools → find sources automatically"). */
    void onAutoFindSources();

private:
    void setupUI();
    void setupMenuBar();
    void buildLanguageMenu();
    void syncThemeActions();
    void retranslateUi();
    /** Rename a quant, moving its provenance entry; content is applied afterwards. */
    void startRename(const QString &oldName, const QString &newName, const QString &content);
    /** Second half of onDeleteModel(): the dialog, once the blob usage is known. */
    void confirmDelete(const QString &modelName, int refCount);

    void closeEvent(QCloseEvent *event) override;

    OllamaCli *m_cli;
    InstallService *m_install;
    TokenManager *m_tokens;
    ProxyService *m_proxy;
    bool m_hideOnClose = false;
    ModelListWidget *m_listWidget;
    ModelEditorWidget *m_editorWidget;
    QTabWidget *m_tabs = nullptr;
    DiscoverPanel *m_ollamaPanel = nullptr;
    QTimer *m_editDebounce = nullptr;
    QString m_pendingRepoId;
    QString m_pendingSelectRepo;
    /** After binding, select the card that now contains this model name. */
    QString m_pendingSelectModel;
    bool m_refreshQueued = false;  // onRefreshAll() coalescing (P-3)
    DiscoverPanel *m_hfPanel = nullptr;
    StartPageHeader *m_hero = nullptr;
    QSplitter *m_splitter;

    QMenu *m_fileMenu;
    QMenu *m_viewMenu = nullptr;
    QMenu *m_themeMenu = nullptr;
    QMenu *m_languageMenu;
    QMenu *m_toolsMenu;
    QMenu *m_helpMenu;
    QAction *m_newAction;
    QAction *m_importAction;
    QAction *m_downloadAction;
    QAction *m_refreshAction;
    QAction *m_quitAction;
    QAction *m_tokensAction;
    QAction *m_proxyAction;
    QAction *m_hfTokenAction = nullptr;
    QAction *m_findSourcesAction;
    QAction *m_aboutAction;
    QActionGroup *m_langGroup;
    QActionGroup *m_themeGroup = nullptr;
    QAction *m_themeSystemAction = nullptr;
    QAction *m_themeLightAction = nullptr;
    QAction *m_themeDarkAction = nullptr;
};

#endif // MAINWINDOW_H
