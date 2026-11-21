#ifndef GGUFIMPORTDIALOG_H
#define GGUFIMPORTDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>

#include "ggufmeta.h"

class GGUFImportDialog : public QDialog
{
    Q_OBJECT
public:
    explicit GGUFImportDialog(QWidget *parent = nullptr);

    QString ggufPath() const;
    QString modelName() const;
    QString generatedModelfile() const;

    // Fill the dialog as if the user had picked `path` in the file chooser.
    void setGgufPath(const QString &path);
    QString metaSummary() const;
    QString templateWarning() const;
    bool templateWarningVisible() const;

private slots:
    void onBrowse();
    void onNameChanged(const QString &text);
    void updatePreview();

private:
    void setupUI();
    void refreshMeta();
    void showMeta(const GgufMeta &meta);

    QLineEdit *m_pathEdit;
    QPushButton *m_browseBtn;
    QLineEdit *m_nameEdit;
    QLabel *m_previewLabel;
    QPlainTextEdit *m_previewEdit;
    QPushButton *m_createBtn;
    QPushButton *m_cancelBtn;

    QGroupBox *m_metaGroup;
    QLabel *m_metaLabel;
    QLabel *m_warnLabel;
};

#endif // GGUFIMPORTDIALOG_H
