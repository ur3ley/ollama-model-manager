#include "ggufimportdialog.h"
#include "modelfilebuilder.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QMessageBox>
#include <QRegularExpressionValidator>
#include <QStringList>
#include "designtokens.h"
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

    m_metaGroup = new QGroupBox(T("gguf.metaGroup"));
    auto *metaLayout = new QVBoxLayout(m_metaGroup);
    m_metaLabel = new QLabel();
    m_metaLabel->setWordWrap(true);
    m_metaLabel->setTextFormat(Qt::PlainText);
    m_metaLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    metaLayout->addWidget(m_metaLabel);
    m_warnLabel = new QLabel();
    m_warnLabel->setWordWrap(true);
    m_warnLabel->setTextFormat(Qt::PlainText);
    tokens::setRole(m_warnLabel, "warning");
    m_warnLabel->setVisible(false);
    metaLayout->addWidget(m_warnLabel);
    m_metaGroup->setVisible(false);
    layout->addWidget(m_metaGroup);

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
    m_createBtn->setProperty("accent", true);
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
    if (!path.isEmpty())
        setGgufPath(path);
}

void GGUFImportDialog::setGgufPath(const QString &path)
{
    m_pathEdit->setText(path);

    QFileInfo fi(path);
    if (m_nameEdit->text().isEmpty())
        m_nameEdit->setText(fi.completeBaseName() + ":latest");

    refreshMeta();
    updatePreview();
}

QString GGUFImportDialog::metaSummary() const
{
    return m_metaLabel->text();
}

QString GGUFImportDialog::templateWarning() const
{
    return m_warnLabel->text();
}

bool GGUFImportDialog::templateWarningVisible() const
{
    // isVisible() is false while the dialog itself is not shown, so report the
    // requested state instead (used by the offline smoke test).
    return !m_warnLabel->isHidden();
}

void GGUFImportDialog::refreshMeta()
{
    const QString path = m_pathEdit->text();
    if (path.isEmpty()) {
        m_metaGroup->setVisible(false);
        return;
    }
    showMeta(GgufMeta::read(path));
}

void GGUFImportDialog::showMeta(const GgufMeta &meta)
{
    m_metaGroup->setVisible(true);
    m_warnLabel->setVisible(false);

    if (!meta.ok) {
        tokens::setRole(m_metaLabel, "danger");
        m_metaLabel->setText(T("gguf.metaError").arg(meta.error));
        return;
    }
    tokens::setRole(m_metaLabel, "");

    const QString paramCount = meta.parameterCount > 0
            ? (meta.parameterCount >= 1000000000ull
                   ? QString::number(meta.parameterCount / 1e9, 'f', 1) + " B"
                   : meta.parameterCount >= 1000000ull
                         ? QString::number(meta.parameterCount / 1e6, 'f', 0) + " M"
                         : QString::number(meta.parameterCount))
            : T("gguf.valueUnknown");

    QStringList lines;
    lines << T("gguf.metaArch").arg(meta.architecture.isEmpty() ? T("gguf.valueUnknown") : meta.architecture);
    lines << T("gguf.metaParams").arg(paramCount);
    lines << T("gguf.metaQuant").arg(meta.quantization.isEmpty() ? T("gguf.valueUnknown") : meta.quantization);
    lines << T("gguf.metaCtx").arg(meta.contextLength > 0
                                   ? QString::number(meta.contextLength)
                                   : T("gguf.valueUnknown"));
    lines << T("gguf.metaTemplate").arg(meta.hasChatTemplate ? T("gguf.tplPresent")
                                                            : T("gguf.tplMissing"));
    m_metaLabel->setText(lines.join('\n'));

    if (meta.templateHasSoftSwitch) {
        m_warnLabel->setText(T("gguf.warnSoftSwitch"));
        m_warnLabel->setVisible(true);
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
        const QString modelfile = ModelfileBuilder::fromLine(path);
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
    return ModelfileBuilder::fromLine(path);
}
