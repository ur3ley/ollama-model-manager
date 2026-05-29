#ifndef RENAMEDIALOG_H
#define RENAMEDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

class RenameDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RenameDialog(const QString &oldName, const QStringList &existingModels,
                          QWidget *parent = nullptr);

    QString newName() const;

private slots:
    void onTextChanged(const QString &text);

private:
    void setupUI(const QString &oldName);

    QLineEdit *m_nameEdit;
    QLabel *m_warningLabel;
    QPushButton *m_renameBtn;
    QPushButton *m_cancelBtn;
    QStringList m_existingModels;
};

#endif // RENAMEDIALOG_H
