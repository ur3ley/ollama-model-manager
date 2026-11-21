#ifndef HFTOKENDIALOG_H
#define HFTOKENDIALOG_H

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class HfClient;

/**
 * "Token…" dialog of the HuggingFace tab: one Hub access token, stored in KDE
 * Wallet (see HfAuth).
 *
 * The field always starts empty — a stored token is never shown back. When one
 * is found the dialog says so and the placeholder reads "задан"; saving
 * replaces it, deleting removes it. Whatever happens, the token is pushed into
 * the HfClient right away, so a gated download can be retried without
 * restarting the application.
 */
class HfTokenDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HfTokenDialog(HfClient *client, QWidget *parent = nullptr);

    /** Did the dialog apply or remove a token? (The caller then re-uses it.) */
    bool changed() const { return m_changed; }

private slots:
    void onSave();
    void onDelete();
    void toggleReveal();

private:
    void refreshState();
    void setStatus(const QString &text, const char *role = "");

    HfClient *m_client;
    QLineEdit *m_edit;
    QPushButton *m_revealBtn;
    QPushButton *m_saveBtn;
    QPushButton *m_deleteBtn;
    QLabel *m_statusLabel;
    bool m_stored = false;
    bool m_changed = false;
};

#endif // HFTOKENDIALOG_H
