#ifndef MODELLISTWIDGET_H
#define MODELLISTWIDGET_H

#include <QWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QProcess>
#include <QLabel>
#include "ollamacli.h"
#include "i18n.h"

// Installed models as hub-style cards: title, last-modified line and size.
class ModelListWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ModelListWidget(OllamaCli *cli, QWidget *parent = nullptr);

    void refresh();
    QString currentModel() const;
    QStringList modelNames() const;

signals:
    void modelSelected(const QString &name);
    void modelsLoaded();
    void modelDoubleClicked(const QString &name);
    void renameRequested(const QString &name);
    void deleteRequested(const QString &name);
    void newModelRequested();
    void importGGUFRequested();

private slots:
    void onItemClicked(QListWidgetItem *item);
    void onItemDoubleClicked(QListWidgetItem *item);
    void onContextMenu(const QPoint &pos);
    void onFilterChanged(const QString &text);
    void onDeleteClicked();

private:
    void setupUI();
    void retranslateUi();
    void populateList(const QList<OllamaCli::ModelInfo> &models);

    OllamaCli *m_cli;
    QLabel *m_title;
    QListWidget *m_list;
    QLineEdit *m_filterEdit;
    QPushButton *m_refreshBtn;
    QPushButton *m_newBtn;
    QPushButton *m_importBtn;
    QPushButton *m_deleteBtn;
    bool m_refreshing = false;
};

#endif // MODELLISTWIDGET_H
