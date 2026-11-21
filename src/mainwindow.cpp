#include "mainwindow.h"
#include "bindsourcedialog.h"
#include "ggufimportdialog.h"
#include "discoverpanel.h"
#include "provenance.h"
#include "registryclient.h"
#include "renamedialog.h"
#include "modelfilebuilder.h"
#include "proxydialog.h"
#include "proxyservice.h"
#include "startpageheader.h"
#include "tokenmanager.h"
#include "tokendialog.h"
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QCloseEvent>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QApplication>
#include <QTemporaryFile>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QProgressDialog>
#include "i18n.h"
#include "theme.h"

namespace {

// Downloaded GGUF sources live next to the app config, not in Ollama's blob
// store: deleting a model does not free them automatically.
QString downloadDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/models");
}

// `model-IQ4_XS-MTP-00001-of-00003.gguf` covers all of its shards; a plain file
// matches itself only.
QString sourceFilePattern(const QString &file)
{
    static const QRegularExpression splitRe(
            QStringLiteral("^(.*)-\\d{5}-of-\\d{5}\\.gguf$"),
            QRegularExpression::CaseInsensitiveOption);
    const QString base = QFileInfo(file).fileName();
    const QRegularExpressionMatch match = splitRe.match(base);
    return match.hasMatch() ? match.captured(1) + QStringLiteral("-*.gguf") : base;
}


} // namespace

MainWindow::MainWindow(TokenManager *tokens, ProxyService *service, QWidget *parent)
    : QMainWindow(parent)
    , m_cli(new OllamaCli(this))
    , m_tokens(tokens)
    , m_proxy(service)
{
    if (!m_tokens)
        m_tokens = new TokenManager(this);
    setupUI();
    setupMenuBar();
    retranslateUi();
    connect(I18n::instance(), &I18n::languageChanged, this, &MainWindow::retranslateUi);

    connect(m_cli, &OllamaCli::createFinished, this, [this](bool success, const QString &output) {
        if (success) {
            statusBar()->showMessage(T("status.created"), 5000);
        } else {
            QMessageBox::warning(this, T("error.title"),
                                 T("error.createFailed").arg(output));
        }
        onRefreshAll();
    });

    connect(m_cli, &OllamaCli::deleteFinished, this, [this](bool success, const QString &output) {
        if (success) {
            statusBar()->showMessage(T("status.deleted"), 3000);
        } else {
            QMessageBox::warning(this, T("error.title"),
                                 T("error.deleteFailed").arg(output));
        }
        onRefreshAll();
    });

    connect(m_cli, &OllamaCli::renameFinished, this,
            [this](bool success, const QString &output) {
        const QString oldName = m_renameOld;
        const QString newName = m_renameNew;
        const QString selectId = m_renameSelectId;
        const QString content = m_renameContent;
        m_renameOld.clear();
        m_renameNew.clear();
        m_renameSelectId.clear();
        m_renameContent.clear();

        if (!success) {
            QMessageBox::warning(this, T("error.title"), T("error.renameFailed").arg(output));
            onRefreshAll();
            return;
        }

        // The original ref is what ties the model to its repository: move the
        // entry to the new name instead of dropping it.
        Provenance::instance().rename(oldName, newName);
        statusBar()->showMessage(T("status.renamed"), 3000);
        m_pendingSelectRepo = selectId;

        if (content.isEmpty()) {
            onRefreshAll();
            return;
        }

        // Rename + new parameters: the data already lives under the new name,
        // so the Modelfile (FROM new name) rebuilds it in place.
        const QString path = writeTempModelfile(content);
        if (path.isEmpty()) {
            QMessageBox::warning(this, T("error.title"), T("error.tmpModelfile"));
            onRefreshAll();
            return;
        }
        statusBar()->showMessage(T("status.updating").arg(newName));
        m_cli->createModelAsync(newName, path);
    });

    connect(m_cli, &OllamaCli::copyFinished, this, [this](bool success, const QString &output) {
        if (success) {
            statusBar()->showMessage(T("status.renamed"), 3000);
        } else {
            QMessageBox::warning(this, T("error.title"),
                                 T("error.renameFailed").arg(output));
        }
        onRefreshAll();
    });

    onRefreshAll();
}

void MainWindow::setupUI()
{
    setWindowTitle(tr("Ollama Model Manager"));
    resize(1180, 760);

    m_tabs = new QTabWidget();
    m_tabs->setDocumentMode(true);
    setCentralWidget(m_tabs);

    auto *installedTab = new QWidget();
    auto *installedLayout = new QVBoxLayout(installedTab);
    installedLayout->setContentsMargins(8, 8, 8, 8);
    installedLayout->setSpacing(8);

    // Hero status strip: totals, Ollama/disk health and the quick actions. The
    // status bar is left for transient messages only.
    m_hero = new StartPageHeader(m_cli, installedTab);
    installedLayout->addWidget(m_hero);

    m_splitter = new QSplitter(Qt::Horizontal);
    installedLayout->addWidget(m_splitter, 1);
    m_tabs->addTab(installedTab, QString());

    m_ollamaPanel = new DiscoverPanel(DiscoverPanel::Source::Ollama, m_cli);
    m_tabs->addTab(m_ollamaPanel, QString());
    m_hfPanel = new DiscoverPanel(DiscoverPanel::Source::HuggingFace, m_cli);
    m_tabs->addTab(m_hfPanel, QString());

    connect(m_ollamaPanel, &DiscoverPanel::modelCreateRequested,
            this, &MainWindow::onCreateModel);
    connect(m_hfPanel, &DiscoverPanel::modelCreateRequested,
            this, &MainWindow::onCreateModel);
    connect(m_ollamaPanel, &DiscoverPanel::installedChanged, this, &MainWindow::onRefreshAll);
    connect(m_hfPanel, &DiscoverPanel::installedChanged, this, &MainWindow::onRefreshAll);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 1)
            m_ollamaPanel->activate();
        else if (index == 2)
            m_hfPanel->activate();
    });

    m_listWidget = new ModelListWidget(m_cli);
    m_listWidget->setMinimumWidth(340);
    m_listWidget->setMaximumWidth(460);
    m_splitter->addWidget(m_listWidget);

    m_editorWidget = new ModelEditorWidget(m_cli);
    m_editorWidget->setMinimumWidth(400);
    m_splitter->addWidget(m_editorWidget);

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({360, 800});

    connect(m_listWidget, &ModelListWidget::repoSelected,
            this, &MainWindow::onRepoSelected);
    connect(m_listWidget, &ModelListWidget::modelsLoaded, this, [this]() {
        m_editorWidget->setAvailableModels(m_listWidget->modelNames());
        m_hero->updateSummary(m_listWidget->summary());
        if (!m_pendingSelectModel.isEmpty()) {
            // After binding, the card may have changed id (local:<name> →
            // ollama:<family>): select it by the installed model name instead.
            const QString name = m_pendingSelectModel;
            m_pendingSelectModel.clear();
            const RepoGroup group = m_listWidget->groupOfModel(name);
            if (!group.id.isEmpty()) {
                m_listWidget->selectRepo(group.id);
                onRepoSelected(group.id);
                return;
            }
        }
        if (!m_pendingSelectRepo.isEmpty()) {
            const QString id = m_pendingSelectRepo;
            m_pendingSelectRepo.clear();
            m_listWidget->selectRepo(id);
            onRepoSelected(id);
            return;
        }
        if (m_editorWidget->repoId().isEmpty()) {
            const QString current = m_listWidget->currentGroupId();
            if (!current.isEmpty())
                onRepoSelected(current);
        }
    });
    connect(m_listWidget, &ModelListWidget::newModelRequested,
            this, &MainWindow::onNewModel);
    connect(m_listWidget, &ModelListWidget::deleteRequested,
            this, &MainWindow::onDeleteModel);
    connect(m_listWidget, &ModelListWidget::renameRequested,
            this, &MainWindow::onRenameModel);
    connect(m_listWidget, &ModelListWidget::bindSourceRequested,
            this, &MainWindow::onBindSourceRequested);
    connect(m_listWidget, &ModelListWidget::importGGUFRequested,
            this, &MainWindow::onImportGGUF);

    connect(m_editorWidget, &ModelEditorWidget::createRequested,
            this, &MainWindow::onCreateModel);
    connect(m_editorWidget, &ModelEditorWidget::updateRequested,
            this, &MainWindow::onUpdateModel);
    connect(m_editorWidget, &ModelEditorWidget::renameRequested,
            this, &MainWindow::onRenameModelDirect);
    connect(m_editorWidget, &ModelEditorWidget::renameAndUpdateRequested,
            this, &MainWindow::onRenameAndUpdateModel);
    connect(m_editorWidget, &ModelEditorWidget::bindSourceRequested,
            this, &MainWindow::onBindSourceRequested);
    connect(m_editorWidget, &ModelEditorWidget::deleteQuantRequested,
            this, &MainWindow::onDeleteModel);

    connect(m_hero, &StartPageHeader::downloadRequested, this, [this]() {
        // "Скачать модель": show the Ollama hub and put the caret into search.
        m_tabs->setCurrentIndex(1);
        m_ollamaPanel->activate();
        m_ollamaPanel->focusSearch();
    });
    m_hero->updateSummary(m_listWidget->summary());
}

void MainWindow::setupMenuBar()
{
    m_fileMenu = menuBar()->addMenu(QString());

    m_newAction = m_fileMenu->addAction(QString(), this, &MainWindow::onNewModel);
    m_newAction->setShortcut(QKeySequence("Ctrl+N"));

    m_importAction = m_fileMenu->addAction(QString(), this, &MainWindow::onImportGGUF);
    m_importAction->setShortcut(QKeySequence("Ctrl+I"));

    m_downloadAction = m_fileMenu->addAction(QString(), this, &MainWindow::onDownloadModel);
    m_downloadAction->setShortcut(QKeySequence("Ctrl+D"));

    m_fileMenu->addSeparator();

    m_refreshAction = m_fileMenu->addAction(QString(), this, &MainWindow::onRefreshAll);
    m_refreshAction->setShortcut(QKeySequence("F5"));

    m_fileMenu->addSeparator();

    m_quitAction = m_fileMenu->addAction(QString(), qApp, &QApplication::quit);
    m_quitAction->setShortcut(QKeySequence("Ctrl+Q"));

    // «Вид → Тема»: системная / светлая / тёмная. Эксклюзивная группа, выбор
    // применяется сразу и запоминается в config.ini (ui/theme).
    m_viewMenu = menuBar()->addMenu(QString());
    m_themeMenu = m_viewMenu->addMenu(QString());
    m_themeGroup = new QActionGroup(this);
    m_themeGroup->setExclusive(true);
    m_themeSystemAction = m_themeMenu->addAction(QString());
    m_themeLightAction = m_themeMenu->addAction(QString());
    m_themeDarkAction = m_themeMenu->addAction(QString());
    m_themeSystemAction->setCheckable(true);
    m_themeLightAction->setCheckable(true);
    m_themeDarkAction->setCheckable(true);
    m_themeGroup->addAction(m_themeSystemAction);
    m_themeGroup->addAction(m_themeLightAction);
    m_themeGroup->addAction(m_themeDarkAction);
    connect(m_themeSystemAction, &QAction::triggered, this,
            []() { theme::Theme::instance()->setMode(theme::Mode::System); });
    connect(m_themeLightAction, &QAction::triggered, this,
            []() { theme::Theme::instance()->setMode(theme::Mode::Light); });
    connect(m_themeDarkAction, &QAction::triggered, this,
            []() { theme::Theme::instance()->setMode(theme::Mode::Dark); });
    connect(theme::Theme::instance(), &theme::Theme::changed, this,
            &MainWindow::syncThemeActions);
    syncThemeActions();

    m_languageMenu = menuBar()->addMenu(QString());
    m_langGroup = new QActionGroup(this);
    m_langGroup->setExclusive(true);
    buildLanguageMenu();

    m_toolsMenu = menuBar()->addMenu(QString());
    m_tokensAction = m_toolsMenu->addAction(QString(), this, &MainWindow::onManageTokens);
    m_tokensAction->setShortcut(QKeySequence("Ctrl+T"));
    m_proxyAction = m_toolsMenu->addAction(QString(), this, &MainWindow::showProxyDialog);
    m_hfTokenAction = m_toolsMenu->addAction(QString(), this, [this]() {
        if (m_hfPanel)
            m_hfPanel->openTokenDialog();
    });
    m_findSourcesAction = m_toolsMenu->addAction(QString(),
                                                 this, &MainWindow::onAutoFindSources);

    m_helpMenu = menuBar()->addMenu(QString());
    m_aboutAction = m_helpMenu->addAction(QString(), this, [this]() {
        QMessageBox::about(this, T("about.title"), T("about.text"));
    });
}

void MainWindow::buildLanguageMenu()
{
    m_languageMenu->clear();
    const QString current = I18n::instance()->currentCode();
    const QStringList codes = I18n::instance()->availableCodes();
    for (const QString &code : codes) {
        QAction *act = m_languageMenu->addAction(I18n::instance()->displayName(code));
        act->setCheckable(true);
        act->setChecked(code == current);
        act->setData(code);
        m_langGroup->addAction(act);
        connect(act, &QAction::triggered, this, [this, code]() {
            if (I18n::instance()->setLanguage(code)) {
                QSettings().setValue("language", code);
            }
        });
    }
}

void MainWindow::syncThemeActions()
{
    if (!m_themeSystemAction)
        return;
    const theme::Mode mode = theme::Theme::instance()->mode();
    m_themeSystemAction->setChecked(mode == theme::Mode::System);
    m_themeLightAction->setChecked(mode == theme::Mode::Light);
    m_themeDarkAction->setChecked(mode == theme::Mode::Dark);
}

void MainWindow::retranslateUi()
{
    setWindowTitle(T("app.title"));
    m_fileMenu->setTitle(T("menu.file"));
    m_viewMenu->setTitle(T("menu.view"));
    m_themeMenu->setTitle(T("menu.theme"));
    m_themeSystemAction->setText(T("menu.themeSystem"));
    m_themeLightAction->setText(T("menu.themeLight"));
    m_themeDarkAction->setText(T("menu.themeDark"));
    m_newAction->setText(T("menu.newModel"));
    m_importAction->setText(T("menu.importGguf"));
    m_downloadAction->setText(T("menu.searchModels"));
    m_tabs->setTabText(0, T("search.tabInstalled"));
    m_tabs->setTabText(1, T("search.tabOllama"));
    m_tabs->setTabText(2, T("search.tabHf"));
    m_refreshAction->setText(T("menu.refresh"));
    m_quitAction->setText(T("menu.quit"));
    m_languageMenu->setTitle(T("menu.language"));
    m_toolsMenu->setTitle(T("menu.tools"));
    m_tokensAction->setText(T("menu.tokens"));
    m_proxyAction->setText(T("menu.proxy"));
    if (m_hfTokenAction)
        m_hfTokenAction->setText(T("menu.hfToken"));
    m_findSourcesAction->setText(T("menu.findSources"));
    m_helpMenu->setTitle(T("menu.help"));
    m_aboutAction->setText(T("menu.about"));
}

void MainWindow::onRefreshAll()
{
    m_listWidget->refresh();
    m_editorWidget->setAvailableModels(m_listWidget->modelNames());
}

void MainWindow::onRepoSelected(const QString &id)
{
    const RepoGroup group = m_listWidget->group(id);
    if (group.id.isEmpty())
        return;

    if (group.source == QLatin1String("hf"))
        statusBar()->showMessage(T("status.sourceHf").arg(group.ref));
    else if (group.source == QLatin1String("ollama"))
        statusBar()->showMessage(T("status.sourceOllama"));
    else
        statusBar()->showMessage(T("status.sourceUnknown"));

    // `ollama show` runs two processes; debounce it so a click paints the list
    // first and rapid clicks do not queue several loads.
    m_pendingRepoId = id;
    if (!m_editDebounce) {
        m_editDebounce = new QTimer(this);
        m_editDebounce->setSingleShot(true);
        m_editDebounce->setInterval(120);
        connect(m_editDebounce, &QTimer::timeout, this, [this]() {
            if (!m_pendingRepoId.isEmpty())
                m_editorWidget->loadRepo(m_listWidget->group(m_pendingRepoId));
        });
    }
    m_editDebounce->start();
}

void MainWindow::showTokensDialog()
{
    TokenDialog dialog(m_tokens, this);
    dialog.exec();
}

void MainWindow::showProxyDialog()
{
    ProxyDialog dialog(m_proxy, this);
    dialog.exec();
}

void MainWindow::onManageTokens()
{
    showTokensDialog();
}

void MainWindow::onBindSourceRequested(const QString &modelName)
{
    const RepoGroup group = m_listWidget->groupOfModel(modelName);
    QString digest;
    for (const InstalledQuant &quant : group.quants) {
        if (quant.modelName == modelName) {
            digest = quant.digest;
            break;
        }
    }

    BindSourceDialog dialog(modelName, digest, this);
    connect(&dialog, &BindSourceDialog::bound, this, [this](const QString &name) {
        statusBar()->showMessage(T("bind.bound").arg(name), 4000);
        // The card id changes with the new source, so reselect by model name
        // once the refreshed list is back.
        m_pendingSelectModel = name;
        onRefreshAll();
    });
    dialog.exec();
}

void MainWindow::onAutoFindSources()
{
    QList<BindTarget> targets;
    const QList<RepoGroup> groups = m_listWidget->groups();
    for (const RepoGroup &group : groups) {
        if (!group.local)
            continue;
        for (const InstalledQuant &quant : group.quants) {
            BindTarget target;
            target.modelName = quant.modelName;
            target.localId = quant.digest;
            targets << target;
        }
    }

    if (targets.isEmpty()) {
        QMessageBox::information(this, T("bind.title"), T("bind.autoNothing"));
        return;
    }

    SourceAutoBinder binder;
    int boundCount = 0;
    QProgressDialog progress(T("bind.autoRunning"), T("browser.cancel"), 0, 0, this);
    progress.setWindowTitle(T("bind.title"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    connect(&binder, &SourceAutoBinder::progress, &progress,
            [&progress](const QString &, int checked, int total) {
        progress.setLabelText(T("bind.autoProgress").arg(checked).arg(total));
        progress.setMaximum(total > 0 ? total : 1);
        progress.setValue(checked);
    });
    connect(&binder, &SourceAutoBinder::bound, this,
            [this](const QString &name, const QString &ref, const QString &) {
        statusBar()->showMessage(T("bind.bound").arg(ref.isEmpty() ? name : ref), 3000);
    });
    connect(&progress, &QProgressDialog::canceled, &binder, &SourceAutoBinder::cancel);
    connect(&binder, &SourceAutoBinder::finished, &progress,
            [&progress, &boundCount](int bound, int) {
        boundCount = bound;
        progress.accept();
    });

    binder.setTargets(targets);
    binder.start();
    // A run without any digest finishes synchronously: no event loop to show.
    if (binder.isRunning())
        progress.exec();

    onRefreshAll();
    QMessageBox::information(this, T("bind.title"),
                             T("bind.autoReport").arg(boundCount).arg(targets.size()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Крестик/Alt+F4 при живом трее — прячем окно: прокси продолжает работать,
    // выход только через «Выход» в меню или трее.
    if (m_hideOnClose) {
        hide();
        event->ignore();
        emit hiddenToTray();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::onNewModel()
{
    m_editorWidget->setCreateMode();
    m_editorWidget->setAvailableModels(m_listWidget->modelNames());
}

void MainWindow::onDeleteModel(const QString &modelName)
{
    QString blob = m_cli->getModelFromBlob(modelName);
    int refCount = m_cli->countBlobReferences(blob);

    QString message;
    QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No;
    QMessageBox::Icon icon = QMessageBox::Question;

    if (refCount > 1) {
        message = T("delete.shared").arg(modelName).arg(refCount - 1);
    } else {
        icon = QMessageBox::Warning;
        message = T("delete.last").arg(modelName);
    }
    message += QStringLiteral("\n\n") + T("delete.onlyQuant");

    // The downloaded GGUF sources are a second full copy; deleting the model
    // always frees them too, unless the import never finished.
    QStringList sourceFiles;
    const Provenance::Entry prov = Provenance::instance().lookup(modelName);
    if (!prov.file.isEmpty()) {
        const QDir dir(downloadDir());
        for (const QString &name : dir.entryList({sourceFilePattern(prov.file)}, QDir::Files))
            sourceFiles << dir.filePath(name);
    }

    QMessageBox box(icon, T("delete.title"), message, buttons, this);
    if (box.exec() == QMessageBox::Yes) {
        // The model may have a provenance record (source repo + file). Drop it
        // together with the model, otherwise the browser keeps claiming the
        // quant is "downloaded" after the model is gone.
        connect(m_cli, &OllamaCli::deleteFinished, this,
                [modelName, sourceFiles](bool ok, const QString &) {
            if (!ok)
                return;
            Provenance::instance().remove(modelName);
            for (const QString &path : sourceFiles) {
                QFile::remove(path);
                QFile::remove(path + QStringLiteral(".part"));
            }
        }, Qt::SingleShotConnection);
        m_cli->deleteModelAsync(modelName);
    }
}

void MainWindow::onRenameModelDirect(const QString &oldName, const QString &newName)
{
    startRename(oldName, newName, QString());
}

void MainWindow::onRenameAndUpdateModel(const QString &oldName, const QString &newName,
                                        const QString &modelfileContent)
{
    const int ret = QMessageBox::question(this, T("update.title"),
                                          T("update.confirmRename").arg(newName),
                                          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes)
        return;
    startRename(oldName, newName, modelfileContent);
}

void MainWindow::startRename(const QString &oldName, const QString &newName,
                             const QString &content)
{
    const RepoGroup group = m_listWidget->groupOfModel(oldName);
    // Grouping is ref-based, so the card id survives a rename — except for a
    // local model, whose id is derived from its current name.
    m_renameSelectId = (group.local && group.quants.size() <= 1)
            ? QStringLiteral("local:") + newName
            : group.id;
    m_renameOld = oldName;
    m_renameNew = newName;
    m_renameContent = content;
    statusBar()->showMessage(T("status.renaming").arg(newName));
    m_cli->renameModelAsync(oldName, newName);
}

void MainWindow::onRenameModel(const QString &modelName)
{
    RenameDialog dialog(modelName, m_listWidget->modelNames(), this);
    if (dialog.exec() == QDialog::Accepted) {
        const QString newName = dialog.newName();
        if (!newName.isEmpty() && newName != modelName)
            onRenameModelDirect(modelName, newName);
    }
}

void MainWindow::onImportGGUF()
{
    GGUFImportDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) {
        QString name = dialog.modelName();
        QString content = dialog.generatedModelfile();
        if (!name.isEmpty() && !content.isEmpty()) {
            onCreateModel(name, content);
        }
    }
}

void MainWindow::onDownloadModel()
{
    m_tabs->setCurrentIndex(m_tabs->currentIndex() == 2 ? 1 : 2);
    if (m_tabs->currentIndex() == 1)
        m_ollamaPanel->activate();
    else
        m_hfPanel->activate();
}

void MainWindow::onCreateModel(const QString &name, const QString &modelfileContent)
{
    QString path = writeTempModelfile(modelfileContent);
    if (path.isEmpty()) {
        QMessageBox::warning(this, T("error.title"), T("error.tmpModelfile"));
        return;
    }
    statusBar()->showMessage(T("status.creating").arg(name));
    m_cli->createModelAsync(name, path);
}

void MainWindow::onUpdateModel(const QString &name, const QString &modelfileContent)
{
    int ret = QMessageBox::question(this, T("update.title"),
                                     T("update.confirm").arg(name),
                                     QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        QString path = writeTempModelfile(modelfileContent);
        if (path.isEmpty()) {
            QMessageBox::warning(this, T("error.title"), T("error.tmpModelfile"));
            return;
        }
        statusBar()->showMessage(T("status.updating").arg(name));
        m_cli->createModelAsync(name, path);
    }
}

QString MainWindow::writeTempModelfile(const QString &content)
{
    QString tempPath = QDir::temp().filePath(
        "ollama-modelfile-" + QString::number(QCoreApplication::applicationPid()) + ".tmp");

    QFile f(tempPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning("Cannot write to %s: %s",
                 qPrintable(tempPath), qPrintable(f.errorString()));
        return QString();
    }
    f.write(content.toUtf8());
    f.close();
    return tempPath;
}
