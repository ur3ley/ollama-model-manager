#include "hftokendialog.h"

#include "designtokens.h"
#include "hfauth.h"
#include "hfclient.h"
#include "i18n.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

HfTokenDialog::HfTokenDialog(HfClient *client, QWidget *parent)
    : QDialog(parent)
    , m_client(client)
{
    setWindowTitle(T("hf.token.title"));
    resize(520, 240);

    auto *hint = new QLabel(this);
    hint->setTextFormat(Qt::RichText);
    hint->setOpenExternalLinks(true);
    hint->setWordWrap(true);
    hint->setText(T("hf.token.hint"));

    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("hfTokenEdit"));
    m_edit->setEchoMode(QLineEdit::Password);
    m_edit->setClearButtonEnabled(true);

    // Small companion button instead of a suffix action: it works with every
    // style and does not need an icon in the theme.
    m_revealBtn = new QPushButton(T("hf.token.show"), this);
    m_revealBtn->setCheckable(true);

    auto *fieldRow = new QHBoxLayout;
    fieldRow->setSpacing(8);
    fieldRow->addWidget(m_edit, 1);
    fieldRow->addWidget(m_revealBtn);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);

    m_saveBtn = new QPushButton(T("hf.token.save"), this);
    m_saveBtn->setProperty("accent", true);
    m_saveBtn->setDefault(true);
    m_deleteBtn = new QPushButton(T("hf.token.delete"), this);
    auto *cancelBtn = new QPushButton(T("hf.token.cancel"), this);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_deleteBtn);
    buttons->addStretch();
    buttons->addWidget(cancelBtn);
    buttons->addWidget(m_saveBtn);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(10);
    layout->addWidget(hint);
    layout->addLayout(fieldRow);
    layout->addWidget(m_statusLabel);
    layout->addLayout(buttons);

    connect(m_saveBtn, &QPushButton::clicked, this, &HfTokenDialog::onSave);
    connect(m_deleteBtn, &QPushButton::clicked, this, &HfTokenDialog::onDelete);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_revealBtn, &QPushButton::toggled, this, &HfTokenDialog::toggleReveal);
    connect(m_edit, &QLineEdit::returnPressed, this, &HfTokenDialog::onSave);

    refreshState();
    m_edit->setFocus(Qt::OtherFocusReason);
}

void HfTokenDialog::setStatus(const QString &text, const char *role)
{
    tokens::setRole(m_statusLabel, role);
    tokens::setCaption(m_statusLabel, role == nullptr || *role == '\0');
    m_statusLabel->setText(text);
}

void HfTokenDialog::refreshState()
{
    // Passive read: the dialog must not block on a wallet password prompt just
    // to show the status line.
    const QString stored = HfAuth::token();
    m_stored = !stored.isEmpty();
    if (m_stored && m_client)
        m_client->setToken(stored);

    m_edit->clear();
    m_edit->setPlaceholderText(m_stored ? T("hf.token.placeholderSet")
                                        : T("hf.token.placeholder"));
    m_deleteBtn->setEnabled(m_stored);

    const bool available = HfAuth::isAvailable();
    if (m_stored) {
        // A token the wallet does not hold can only come from this session;
        // never claim it was saved then.
        setStatus(available ? T("hf.token.stored") : T("hf.token.sessionOnly"),
                  available ? "success" : "warning");
    } else if (!available) {
        // No wallet daemon: the token still works for this session, say so
        // before the user types anything.
        setStatus(T("hf.token.walletUnavailable"), "warning");
    } else {
        setStatus(T("hf.token.notStored"));
    }
}

void HfTokenDialog::toggleReveal()
{
    const bool shown = m_revealBtn->isChecked();
    m_edit->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password);
    m_revealBtn->setText(shown ? T("hf.token.hide") : T("hf.token.show"));
}

void HfTokenDialog::onSave()
{
    const QString value = HfClient::normalizeToken(m_edit->text());
    if (value.isEmpty()) {
        setStatus(T("hf.token.missing"), "danger");
        return;
    }

    // Apply first: the download must work even if the wallet is locked.
    if (m_client)
        m_client->setToken(value);

    QString error;
    const bool saved = HfAuth::setToken(value, &error);
    m_stored = true;
    m_changed = true;
    m_edit->clear();
    m_edit->setPlaceholderText(T("hf.token.placeholderSet"));
    m_deleteBtn->setEnabled(true);
    if (saved)
        setStatus(T("hf.token.saved"), "success");
    else
        setStatus(T("hf.token.saveFailed").arg(error), "danger");
}

void HfTokenDialog::onDelete()
{
    if (m_client)
        m_client->setToken(QString());

    QString error;
    const bool removed = HfAuth::clear(&error);
    m_stored = false;
    m_changed = true;
    m_edit->clear();
    m_edit->setPlaceholderText(T("hf.token.placeholder"));
    m_deleteBtn->setEnabled(false);

    if (removed)
        setStatus(T("hf.token.deleted"), "success");
    else
        setStatus(T("hf.token.deleteFailed").arg(error), "danger");
}
