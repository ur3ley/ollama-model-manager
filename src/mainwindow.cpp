#include "mainwindow.h"
#include "ggufimportdialog.h"
#include "renamedialog.h"
#include "modelfilebuilder.h"
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QStatusBar>
#include <QApplication>
#include <QTemporaryFile>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include "i18n.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_cli = new OllamaCli(this);
    setupUI();
    setupMenuBar();
    retranslateUi();
    connect(I18n::instance(), &I18n::languageChanged, this, &MainWindow::retranslateUi);
    checkOllamaAvailable();

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
    resize(920, 620);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    setCentralWidget(m_splitter);

    m_listWidget = new ModelListWidget(m_cli);
    m_listWidget->setMinimumWidth(260);
    m_listWidget->setMaximumWidth(400);
    m_splitter->addWidget(m_listWidget);

    m_editorWidget = new ModelEditorWidget(m_cli);
    m_editorWidget->setMinimumWidth(400);
    m_splitter->addWidget(m_editorWidget);

    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setSizes({280, 640});

    connect(m_listWidget, &ModelListWidget::modelSelected,
            this, &MainWindow::onModelSelected);
    connect(m_listWidget, &ModelListWidget::newModelRequested,
            this, &MainWindow::onNewModel);
    connect(m_listWidget, &ModelListWidget::deleteRequested,
            this, &MainWindow::onDeleteModel);
    connect(m_listWidget, &ModelListWidget::renameRequested,
            this, &MainWindow::onRenameModel);
    connect(m_listWidget, &ModelListWidget::importGGUFRequested,
            this, &MainWindow::onImportGGUF);

    connect(m_editorWidget, &ModelEditorWidget::createRequested,
            this, &MainWindow::onCreateModel);
    connect(m_editorWidget, &ModelEditorWidget::updateRequested,
            this, &MainWindow::onUpdateModel);
}

void MainWindow::setupMenuBar()
{
    m_fileMenu = menuBar()->addMenu(QString());

    m_newAction = m_fileMenu->addAction(QString(), this, &MainWindow::onNewModel);
    m_newAction->setShortcut(QKeySequence("Ctrl+N"));

    m_importAction = m_fileMenu->addAction(QString(), this, &MainWindow::onImportGGUF);
    m_importAction->setShortcut(QKeySequence("Ctrl+I"));

    m_fileMenu->addSeparator();

    m_refreshAction = m_fileMenu->addAction(QString(), this, &MainWindow::onRefreshAll);
    m_refreshAction->setShortcut(QKeySequence("F5"));

    m_fileMenu->addSeparator();

    m_quitAction = m_fileMenu->addAction(QString(), qApp, &QApplication::quit);
    m_quitAction->setShortcut(QKeySequence("Ctrl+Q"));

    m_languageMenu = menuBar()->addMenu(QString());
    m_langGroup = new QActionGroup(this);
    m_langGroup->setExclusive(true);
    buildLanguageMenu();

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

void MainWindow::retranslateUi()
{
    setWindowTitle(T("app.title"));
    m_fileMenu->setTitle(T("menu.file"));
    m_newAction->setText(T("menu.newModel"));
    m_importAction->setText(T("menu.importGguf"));
    m_refreshAction->setText(T("menu.refresh"));
    m_quitAction->setText(T("menu.quit"));
    m_languageMenu->setTitle(T("menu.language"));
    m_helpMenu->setTitle(T("menu.help"));
    m_aboutAction->setText(T("menu.about"));
}

void MainWindow::checkOllamaAvailable()
{
    if (!m_cli->isAvailable()) {
        statusBar()->showMessage(T("status.ollamaMissing"));
    } else {
        statusBar()->showMessage(T("status.ollamaFound").arg(m_cli->ollamaPath()));
    }
}

void MainWindow::onRefreshAll()
{
    m_listWidget->refresh();
    m_editorWidget->setAvailableModels(m_listWidget->modelNames());
}

void MainWindow::onModelSelected(const QString &modelName)
{
    m_editorWidget->loadModel(modelName);
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

    int ret = QMessageBox(icon, T("delete.title"), message, buttons, this).exec();
    if (ret == QMessageBox::Yes) {
        m_cli->deleteModelAsync(modelName);
    }
}

void MainWindow::onRenameModel(const QString &modelName)
{
    RenameDialog dialog(modelName, m_listWidget->modelNames(), this);
    if (dialog.exec() == QDialog::Accepted) {
        QString newName = dialog.newName();
        if (!newName.isEmpty() && newName != modelName) {
            m_cli->copyModelAsync(modelName, newName);
        }
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
