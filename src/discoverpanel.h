#ifndef DISCOVERPANEL_H
#define DISCOVERPANEL_H

#include <QList>
#include <QSet>
#include <QWidget>

#include "hfclient.h"
#include "modelfilters.h"
#include "registryclient.h"

class OllamaCli;
class ModelBrowserPanel;
class QComboBox;
class QLineEdit;
class QPushButton;

// One source tab of the hub: search + sort + filters + card list.
//
// Source::Ollama browses ollama.com and installs via `ollama pull`;
// Source::HuggingFace browses GGUF repositories, downloads a quant and asks the
// host window to create the model (`modelCreateRequested`).
class DiscoverPanel : public QWidget
{
    Q_OBJECT
public:
    enum class Source { Ollama, HuggingFace };

    explicit DiscoverPanel(Source source, OllamaCli *cli, QWidget *parent = nullptr);

    /** Load the initial "popular" list. */
    void activate();

    // Hooks for the offscreen smoke test.
    void searchAs(const QString &query);
    void selectModel(int row);
    void selectTag(int row);
    int modelCount() const;
    int tagCount() const;
    QString suggestedName() const;
    QString statusText() const;
    void startInstall();

signals:
    void modelCreateRequested(const QString &name, const QString &modelfileContent);
    void installedChanged();

private slots:
    void onSearch();
    void onItemSelected(const QString &itemId);
    void onAction(const QString &itemId, const QString &variantId);
    void onCancel();
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished(bool ok, const QString &path, const QString &error);
    void onPullProgress(const QString &status, int percent);
    void onPullFinished(bool ok, const QString &output);

private:
    void setupUi();
    void rebuildItems();
    void openFilters();
    void updateFilterButton();
    void refreshInstalled();
    void applySort(QList<RegistryClient::Model> &models) const;
    QString hfSortKey() const;

    bool isOllama() const { return m_source == Source::Ollama; }

    Source m_source;
    OllamaCli *m_cli;
    RegistryClient *m_registry;
    HfClient *m_hf;

    QLineEdit *m_queryEdit;
    QPushButton *m_searchBtn;
    QComboBox *m_sortCombo;
    QComboBox *m_hfSortCombo;
    QPushButton *m_filterBtn;
    ModelBrowserPanel *m_panel;

    QList<RegistryClient::Model> m_ollamaResults;
    QList<HfClient::Repo> m_hfResults;
    ModelFilters m_filters;
    QSet<QString> m_installedNames;
    QString m_pendingItemId;
    QString m_pendingRepo;
    QString m_pendingFile;
    QString m_pullingName;
    bool m_loaded = false;
};

#endif // DISCOVERPANEL_H
