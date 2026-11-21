#include "tokendialog.h"

#include "designtokens.h"
#include "i18n.h"
#include "tokenmanager.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

TokenDialog::TokenDialog(TokenManager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(T("tokens.title"));
    resize(620, 420);

    m_endpointLabel = new QLabel(this);
    m_endpointLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_endpointLabel->setWordWrap(true);

    m_table = new QTableWidget(0, 2, this);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setHorizontalHeaderLabels({T("tokens.device"), T("tokens.created")});
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        const bool hasSelection = !selectedDevice().isEmpty();
        m_revokeButton->setEnabled(hasSelection);
        m_showButton->setEnabled(hasSelection);
        m_copyButton->setEnabled(hasSelection);
    });
    connect(m_table, &QTableWidget::itemDoubleClicked, this, &TokenDialog::onCopy);

    m_issueButton = new QPushButton(T("tokens.issue"), this);
    m_revokeButton = new QPushButton(T("tokens.revoke"), this);
    m_showButton = new QPushButton(T("tokens.show"), this);
    m_copyButton = new QPushButton(T("tokens.copy"), this);
    m_copyUrlButton = new QPushButton(T("tokens.copyUrl"), this);
    m_refreshButton = new QPushButton(T("tokens.refresh"), this);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_issueButton);
    buttons->addWidget(m_revokeButton);
    buttons->addWidget(m_showButton);
    buttons->addWidget(m_copyButton);
    buttons->addStretch();
    buttons->addWidget(m_copyUrlButton);
    buttons->addWidget(m_refreshButton);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_endpointLabel);
    layout->addWidget(m_table, 1);
    layout->addLayout(buttons);
    layout->addWidget(m_statusLabel);

    connect(m_issueButton, &QPushButton::clicked, this, &TokenDialog::onIssue);
    connect(m_revokeButton, &QPushButton::clicked, this, &TokenDialog::onRevoke);
    connect(m_showButton, &QPushButton::clicked, this, &TokenDialog::onShow);
    connect(m_copyButton, &QPushButton::clicked, this, &TokenDialog::onCopy);
    connect(m_copyUrlButton, &QPushButton::clicked, this, &TokenDialog::onCopyUrl);
    connect(m_refreshButton, &QPushButton::clicked, this, &TokenDialog::refresh);

    m_endpointLabel->setText(T("tokens.endpoint").arg(TokenManager::endpoint()));
    refresh();
}

QString TokenDialog::selectedDevice() const
{
    const auto rows = m_table->selectionModel()->selectedRows();
    if (rows.isEmpty())
        return QString();
    QTableWidgetItem *item = m_table->item(rows.first().row(), 0);
    return item ? item->text() : QString();
}

void TokenDialog::setStatus(const QString &text, bool error)
{
    tokens::setRole(m_statusLabel, error ? "danger" : "");
    m_statusLabel->setText(text);
}

void TokenDialog::refresh()
{
    QString error;
    const QList<TokenManager::Entry> entries = m_manager->entries(&error);

    m_table->setRowCount(0);
    for (const TokenManager::Entry &entry : entries) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        // Wallet stores ISO-8601; show something readable, keep raw on failure.
        const QDateTime created = QDateTime::fromString(entry.created, Qt::ISODate);
        const QString createdText =
            created.isValid() ? created.toString(QStringLiteral("yyyy-MM-dd HH:mm")) : entry.created;
        m_table->setItem(row, 0, new QTableWidgetItem(entry.device));
        m_table->setItem(row, 1, new QTableWidgetItem(createdText));
    }
    m_table->resizeColumnToContents(0);
    m_revokeButton->setEnabled(false);
    m_showButton->setEnabled(false);
    m_copyButton->setEnabled(false);

    if (!error.isEmpty()) {
        setStatus(T("tokens.walletError").arg(error), true);
    } else if (entries.isEmpty()) {
        setStatus(T("tokens.emptyList"));
    } else {
        setStatus(QString());
    }
}

void TokenDialog::onIssue()
{
    bool ok = false;
    const QString device = QInputDialog::getText(this, T("tokens.title"), T("tokens.askDevice"),
                                                QLineEdit::Normal, QString(), &ok)
                               .trimmed();
    if (!ok || device.isEmpty())
        return;

    QString token;
    QString error;
    if (!m_manager->issue(device, &token, &error)) {
        setStatus(T("tokens.walletError").arg(error), true);
        return;
    }

    // Прокси подхватит новый токен по сигналу TokenManager::tokensChanged,
    // отдельная синхронизация (sync) больше не нужна.
    refresh();

    QMessageBox box(this);
    box.setWindowTitle(T("tokens.newTokenTitle").arg(device));
    box.setIcon(QMessageBox::Information);
    box.setText(T("tokens.newTokenHint"));
    box.setInformativeText(token);
    if (QLabel *label = box.findChild<QLabel *>(QStringLiteral("qt_msgbox_informativelabel")))
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();

    setStatus(T("tokens.issued").arg(device));
}

void TokenDialog::onRevoke()
{
    const QString device = selectedDevice();
    if (device.isEmpty()) {
        setStatus(T("tokens.noSelection"), true);
        return;
    }
    if (QMessageBox::question(this, T("tokens.title"), T("tokens.revokeConfirm").arg(device))
        != QMessageBox::Yes)
        return;

    QString error;
    if (!m_manager->revoke(device, &error)) {
        setStatus(T("tokens.walletError").arg(error), true);
        return;
    }
    refresh();
    setStatus(T("tokens.revoked").arg(device));
}

void TokenDialog::onShow()
{
    const QString device = selectedDevice();
    if (device.isEmpty()) {
        setStatus(T("tokens.noSelection"), true);
        return;
    }
    QString value;
    QString error;
    if (!m_manager->token(device, &value, &error)) {
        setStatus(T("tokens.walletError").arg(error), true);
        return;
    }
    setStatus(T("tokens.shown").arg(device).arg(value));
}

void TokenDialog::onCopy()
{
    const QString device = selectedDevice();
    if (device.isEmpty()) {
        setStatus(T("tokens.noSelection"), true);
        return;
    }
    QString value;
    QString error;
    if (!m_manager->token(device, &value, &error)) {
        setStatus(T("tokens.walletError").arg(error), true);
        return;
    }
    QApplication::clipboard()->setText(value);
    setStatus(T("tokens.copied").arg(device));
}

void TokenDialog::onCopyUrl()
{
    QApplication::clipboard()->setText(TokenManager::endpoint());
    setStatus(T("tokens.urlCopied"));
}
