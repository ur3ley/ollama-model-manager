#include "renamedialog.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QRegularExpressionValidator>
#include "i18n.h"

RenameDialog::RenameDialog(const QString &oldName, const QStringList &existingModels,
                           QWidget *parent)
    : QDialog(parent), m_existingModels(existingModels)
{
    setupUI(oldName);
}

void RenameDialog::setupUI(const QString &oldName)
{
    setWindowTitle(T("rename.title"));
    setMinimumWidth(400);

    auto *layout = new QVBoxLayout(this);

    auto *infoLabel = new QLabel(T("rename.info").arg(oldName));
    layout->addWidget(infoLabel);

    auto *formLayout = new QFormLayout();
    m_nameEdit = new QLineEdit();
    m_nameEdit->setPlaceholderText(T("rename.placeholder"));
    m_nameEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[a-zA-Z0-9._/:\\-]*"), m_nameEdit));
    formLayout->addRow(T("rename.newName"), m_nameEdit);
    layout->addLayout(formLayout);

    m_warningLabel = new QLabel();
    m_warningLabel->setStyleSheet("color: red; font-size: 11px;");
    m_warningLabel->setVisible(false);
    layout->addWidget(m_warningLabel);

    layout->addStretch();

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_cancelBtn = new QPushButton(T("common.cancel"));
    btnLayout->addWidget(m_cancelBtn);
    m_renameBtn = new QPushButton(T("rename.rename"));
    m_renameBtn->setEnabled(false);
    m_renameBtn->setStyleSheet("QPushButton { font-weight: bold; }");
    btnLayout->addWidget(m_renameBtn);
    layout->addLayout(btnLayout);

    connect(m_nameEdit, &QLineEdit::textChanged, this, &RenameDialog::onTextChanged);
    connect(m_renameBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void RenameDialog::onTextChanged(const QString &text)
{
    bool valid = !text.trimmed().isEmpty();
    bool exists = m_existingModels.contains(text.trimmed());

    if (exists) {
        m_warningLabel->setText(T("rename.exists"));
        m_warningLabel->setVisible(true);
        valid = false;
    } else {
        m_warningLabel->setVisible(false);
    }

    m_renameBtn->setEnabled(valid);
}

QString RenameDialog::newName() const
{
    return m_nameEdit->text().trimmed();
}
