#include "ggufimportdialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include "i18n.h"

GGUFImportDialog::GGUFImportDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUI();
}

void GGUFImportDialog::setupUI()
{
    setWindowTitle(T("gguf.title"));
    setMinimumWidth(480);

    auto *layout = new QVBoxLayout(this);

    auto *fileGroup = new QGroupBox(T("gguf.fileGroup"));
    auto *fileLayout = new QHBoxLayout(fileGroup);
    m_pathEdit = new QLineEdit();
    m_pathEdit->setPlaceholderText(T("gguf.pathPlaceholder"));
    m_pathEdit->setReadOnly(true);
    fileLayout->addWidget(m_pathEdit);
    m_browseBtn = new QPushButton(T("gguf.browse"));
    fileLayout->addWidget(m_browseBtn);
    layout->addWidget(fileGroup);

    auto *nameGroup = new QGroupBox(T("gguf.nameGroup"));
    auto *nameLayout = new QFormLayout(nameGroup);
    m_nameEdit = new QLineEdit();
    m_nameEdit->setPlaceholderText(T("gguf.namePlaceholder"));
    m_nameEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[a-zA-Z0-9._/:\\-]*"), m_nameEdit));
    nameLayout->addRow(T("gguf.name"), m_nameEdit);
    layout->addWidget(nameGroup);

    auto *previewGroup = new QGroupBox(T("gguf.previewGroup"));
    auto *previewLayout = new QVBoxLayout(previewGroup);
    m_previewLabel = new QLabel();
    previewLayout->addWidget(m_previewLabel);
    m_previewEdit = new QPlainTextEdit();
    m_previewEdit->setReadOnly(true);
    m_previewEdit->setMaximumHeight(120);
    previewLayout->addWidget(m_previewEdit);
    layout->addWidget(previewGroup);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_cancelBtn = new QPushButton(T("common.cancel"));
    btnLayout->addWidget(m_cancelBtn);
    m_createBtn = new QPushButton(T("gguf.create"));
    m_createBtn->setStyleSheet("QPushButton { font-weight: bold; }");
    m_createBtn->setEnabled(false);
    btnLayout->addWidget(m_createBtn);
    layout->addLayout(btnLayout);

    connect(m_browseBtn, &QPushButton::clicked, this, &GGUFImportDialog::onBrowse);
    connect(m_nameEdit, &QLineEdit::textChanged, this, &GGUFImportDialog::onNameChanged);
    connect(m_createBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void GGUFImportDialog::onBrowse()
{
    QString path = QFileDialog::getOpenFileName(this, T("gguf.chooseFile"),
                                                 QString(),
                                                 T("gguf.fileFilter"));
    if (!path.isEmpty()) {
        m_pathEdit->setText(path);

        QFileInfo fi(path);
        QString baseName = fi.completeBaseName();
        if (m_nameEdit->text().isEmpty()) {
            m_nameEdit->setText(baseName + ":latest");
        }
        updatePreview();
    }
}

void GGUFImportDialog::onNameChanged(const QString &)
{
    updatePreview();
}

void GGUFImportDialog::updatePreview()
{
    QString path = m_pathEdit->text();
    QString name = m_nameEdit->text().trimmed();

    bool ready = !path.isEmpty() && !name.isEmpty();
    m_createBtn->setEnabled(ready);

    if (ready) {
        QString modelfile = "FROM " + path + "\n";
        m_previewEdit->setPlainText(modelfile);
        m_previewLabel->setText(T("gguf.willCreate").arg(name));
    } else {
        m_previewEdit->clear();
        m_previewLabel->setText(T("gguf.selectHint"));
    }
}

QString GGUFImportDialog::ggufPath() const
{
    return m_pathEdit->text();
}

QString GGUFImportDialog::modelName() const
{
    return m_nameEdit->text().trimmed();
}

QString GGUFImportDialog::generatedModelfile() const
{
    QString path = m_pathEdit->text();
    if (path.isEmpty()) return QString();
    return "FROM " + path + "\n";
}
