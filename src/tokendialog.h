#ifndef TOKENDIALOG_H
#define TOKENDIALOG_H

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
class TokenManager;

/** API token management: one token per device, stored in KDE Wallet. */
class TokenDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TokenDialog(TokenManager *manager, QWidget *parent = nullptr);

private slots:
    void refresh();
    void onIssue();
    void onRevoke();
    void onShow();
    void onCopy();
    void onCopyUrl();
    void onAdminToggled(QTableWidgetItem *item);

private:
    QString selectedDevice() const;
    void setStatus(const QString &text, bool error = false);

    TokenManager *m_manager;
    QString m_endpoint;  // [proxy] public_url or derived from the listen address
    QTableWidget *m_table;
    QPushButton *m_issueButton;
    QPushButton *m_revokeButton;
    QPushButton *m_showButton;
    QPushButton *m_copyButton;
    QPushButton *m_copyUrlButton;
    QPushButton *m_refreshButton;
    QLabel *m_policyLabel = nullptr;
    QLabel *m_endpointLabel;
    QLabel *m_statusLabel;
};

#endif // TOKENDIALOG_H
