#ifndef MODELLISTWIDGET_H
#define MODELLISTWIDGET_H

#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

#include "i18n.h"
#include "ollamacli.h"
#include "repogroup.h"

// Numbers for the hero block above the list: how many quants are installed,
// how many repository cards they form and how much space they take together.
struct InstalledSummary {
    int quants = 0;
    int repositories = 0;
    quint64 bytes = 0;
};

// Installed models as repository cards: one card per repository (an Ollama
// family, a HuggingFace repo or a locally created model), with every installed
// quant kept inside the card instead of one card per tag.
class ModelListWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ModelListWidget(OllamaCli *cli, QWidget *parent = nullptr);

    void refresh();
    /** Totals over the currently loaded groups (no CLI call). */
    InstalledSummary summary() const;
    /** Current name of the first quant of the selected repository. */
    QString currentModel() const;
    /** Every installed model name (all quants), e.g. for name-conflict checks. */
    QStringList modelNames() const;

    RepoGroup group(const QString &id) const;
    RepoGroup groupOfModel(const QString &modelName) const;
    QString currentGroupId() const;
    /** All repository cards currently shown (for batch source lookup). */
    QList<RepoGroup> groups() const { return m_groups; }

    /** Select a repository card without emitting repoSelected(). */
    void selectRepo(const QString &id);

signals:
    void repoSelected(const QString &id);
    void modelsLoaded();
    void renameRequested(const QString &modelName);
    void deleteRequested(const QString &modelName);
    /** Local card: open "bind source…" for this installed model. */
    void bindSourceRequested(const QString &modelName);
    void newModelRequested();
    void importGGUFRequested();

private slots:
    void onContextMenu(QPoint pos);
    void onFilterChanged(const QString &text);

private:
    void setupUI();
    void retranslateUi();
    void populateList(const QList<OllamaCli::ModelInfo> &models);
    void applyGroups(bool notify);
    QString subtitleFor(const RepoGroup &group) const;
    QString statsFor(const RepoGroup &group) const;

    OllamaCli *m_cli;
    QLabel *m_title;
    QListWidget *m_list;
    QLineEdit *m_filterEdit;
    QPushButton *m_refreshBtn;
    QToolButton *m_deleteBtn;
    QList<RepoGroup> m_groups;
    QList<OllamaCli::ModelInfo> m_models;  // last `ollama list`, regrouped on provenance changes
    bool m_regroupQueued = false;
    bool m_refreshing = false;
    // A refresh asked for while one runs: that list may predate the change.
    bool m_refreshAgain = false;
};

#endif // MODELLISTWIDGET_H
