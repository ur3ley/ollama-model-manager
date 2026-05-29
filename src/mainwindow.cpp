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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_cli = new OllamaCli(this);
    setupUI();
    setupMenuBar();
    checkOllamaAvailable();

    connect(m_cli, &OllamaCli::createFinished, this, [this](bool success, const QString &output) {
        if (success) {
            statusBar()->showMessage(tr("Модель создана."), 5000);
        } else {
            QMessageBox::warning(this, tr("Ошибка"),
                                 tr("Не удалось создать модель:\n%1").arg(output));
        }
        onRefreshAll();
    });

    connect(m_cli, &OllamaCli::deleteFinished, this, [this](bool success, const QString &output) {
        if (success) {
            statusBar()->showMessage(tr("Модель удалена."), 3000);
        } else {
            QMessageBox::warning(this, tr("Ошибка"),
                                 tr("Не удалось удалить модель:\n%1").arg(output));
        }
        onRefreshAll();
    });

    connect(m_cli, &OllamaCli::copyFinished, this, [this](bool success, const QString &output) {
        if (success) {
            statusBar()->showMessage(tr("Модель переименована."), 3000);
        } else {
            QMessageBox::warning(this, tr("Ошибка"),
                                 tr("Не удалось переименовать модель:\n%1").arg(output));
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
    auto *fileMenu = menuBar()->addMenu(tr("&Файл"));

    auto *newAction = fileMenu->addAction(tr("&Новая модель"), this, &MainWindow::onNewModel);
    newAction->setShortcut(QKeySequence("Ctrl+N"));

    auto *importAction = fileMenu->addAction(tr("&Импорт GGUF..."), this, &MainWindow::onImportGGUF);
    importAction->setShortcut(QKeySequence("Ctrl+I"));

    fileMenu->addSeparator();

    auto *refreshAction = fileMenu->addAction(tr("&Обновить"), this, &MainWindow::onRefreshAll);
    refreshAction->setShortcut(QKeySequence("F5"));

    fileMenu->addSeparator();

    auto *quitAction = fileMenu->addAction(tr("&Выход"), qApp, &QApplication::quit);
    quitAction->setShortcut(QKeySequence("Ctrl+Q"));

    auto *helpMenu = menuBar()->addMenu(tr("&Справка"));
    helpMenu->addAction(tr("&О программе"), this, [this]() {
        QMessageBox::about(this, tr("О программе"),
                           tr("Ollama Model Manager v1.0\n\n"
                              "Управление моделями Ollama.\n"
                              "Создание, редактирование, переименование, импорт GGUF."));
    });
}

void MainWindow::checkOllamaAvailable()
{
    if (!m_cli->isAvailable()) {
        statusBar()->showMessage(tr("Внимание: ollama не найден в PATH"));
    } else {
        statusBar()->showMessage(tr("Ollama найден: %1").arg(m_cli->ollamaPath()));
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
        message = tr("Удалить \"%1\"?\n\n"
                     "На этот blob ссылается ещё %n модель(ей). "
                     "Будет удалён только этот псевдоним — данные модели сохранятся.",
                     nullptr, refCount - 1).arg(modelName);
    } else {
        icon = QMessageBox::Warning;
        message = tr("Удалить \"%1\"?\n\n"
                     "ВНИМАНИЕ: это ПОСЛЕДНЯЯ ссылка на данные модели.\n"
                     "Файлы модели (blobs) будут БЕЗВОЗВРАТНО УДАЛЕНЫ!").arg(modelName);
    }

    int ret = QMessageBox(icon, tr("Удаление модели"), message, buttons, this).exec();
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
        QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось записать временный Modelfile."));
        return;
    }
    statusBar()->showMessage(tr("Создание модели: %1...").arg(name));
    m_cli->createModelAsync(name, path);
}

void MainWindow::onUpdateModel(const QString &name, const QString &modelfileContent)
{
    int ret = QMessageBox::question(this, tr("Обновление модели"),
                                     tr("Пересоздать модель \"%1\" с новыми параметрами?\n"
                                        "Существующая модель будет перезаписана.").arg(name),
                                     QMessageBox::Yes | QMessageBox::No,
                                     QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        QString path = writeTempModelfile(modelfileContent);
        if (path.isEmpty()) {
            QMessageBox::warning(this, tr("Ошибка"), tr("Не удалось записать временный Modelfile."));
            return;
        }
        statusBar()->showMessage(tr("Обновление модели: %1...").arg(name));
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
