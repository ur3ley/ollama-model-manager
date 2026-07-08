#ifndef MODELLISTWIDGET_H
#define MODELLISTWIDGET_H

#include <QWidget>
#include <QTreeWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QProcess>
#include <QLabel>
#include "ollamacli.h"
#include "i18n.h"

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
    void modelDoubleClicked(const QString &name);
    void renameRequested(const QString &name);
    void deleteRequested(const QString &name);
    void newModelRequested();
    void importGGUFRequested();

private slots:
    void onItemClicked(QTreeWidgetItem *item, int column);
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onContextMenu(const QPoint &pos);
    void onFilterChanged(const QString &text);
    void onDeleteClicked();

private:
    void setupUI();
    void retranslateUi();
    void populateList(const QList<OllamaCli::ModelInfo> &models);

    OllamaCli *m_cli;
    QTreeWidget *m_tree;
    QLineEdit *m_filterEdit;
    QLabel *m_title;
    QPushButton *m_refreshBtn;
    QPushButton *m_newBtn;
    QPushButton *m_importBtn;
    QPushButton *m_deleteBtn;
};

#endif // MODELLISTWIDGET_H
