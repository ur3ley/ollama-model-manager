#ifndef GGUFIMPORTDIALOG_H
#define GGUFIMPORTDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>

class GGUFImportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit GGUFImportDialog(QWidget *parent = nullptr);

    QString ggufPath() const;
    QString modelName() const;
    QString generatedModelfile() const;

private slots:
    void onBrowse();
    void onNameChanged(const QString &text);
    void updatePreview();

private:
    void setupUI();

    QLineEdit *m_pathEdit;
    QPushButton *m_browseBtn;
    QLineEdit *m_nameEdit;
    QLabel *m_previewLabel;
    QPlainTextEdit *m_previewEdit;
    QPushButton *m_createBtn;
    QPushButton *m_cancelBtn;
};

#endif // GGUFIMPORTDIALOG_H
