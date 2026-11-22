#ifndef DISCOVERPANEL_H
#define DISCOVERPANEL_H

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include "hfclient.h"
#include "hfvariants.h"
#include "modelfilters.h"
#include "ollamacli.h"
#include "registryclient.h"

class InstallService;
class ModelBrowserPanel;
class QComboBox;
class QLineEdit;
class QPushButton;

// One source tab of the hub: search + sort + filters + card list.
//
// Source::Ollama browses ollama.com and installs via `ollama pull`;
// Source::HuggingFace browses GGUF repositories, downloads a quant and imports
// it with `ollama create`. Installation itself (provenance, cleanup) is
// InstallService's job; the panel shows its OllamaJob (A-1, A-2).
class DiscoverPanel : public QWidget
{
    Q_OBJECT
public:
    enum class Source { Ollama, HuggingFace };

    explicit DiscoverPanel(Source source, InstallService *install, QWidget *parent = nullptr);

    /** HuggingFace token dialog (opened from the Tools menu). */
    void openTokenDialog();

    /** Load the initial "popular" list. */
    void activate();

    /** Put the keyboard focus into the query field (hero "download" action). */
    void focusSearch();

    // Hooks for the offscreen smoke test.
    void searchAs(const QString &query);
    void selectModel(int row);
    void selectTag(int row);
    int modelCount() const;
    int tagCount() const;
    QString suggestedName() const;
    QString statusText() const;
    QString selectedVariantText() const;
    void cancelSelectedVariant();
    void startInstall();
    /** HF import entry for the global downloads panel ("" when idle). */
    QString hfActivityText() const;
    bool hfCancellable() const;
    void cancelHfActivity();
    /** Rows for the global downloads panel (running first, then the queue). */
    struct HfTaskInfo {
        QString id;
        QString label;
        bool active = false;
        bool cancellable = false;
        bool creating = false;
    };
    QList<HfTaskInfo> hfTasks() const;
    bool cancelHfTask(const QString &id);
    /** Leftover partial imports (unexpected stop) for the downloads manager. */
    struct HfIncomplete {
        QString sidecar;
        QString label;
        qint64 downloaded = 0;
        qint64 total = 0;
    };
    QList<HfIncomplete> hfIncomplete() const;
    bool resumeHfIncomplete(const QString &sidecar);
    bool removeHfIncomplete(const QString &sidecar);
    /** Old leftover `.part` without metadata: delete-only cleanup. */
    bool removeHfPartial(const QString &partPath);
    /** Folder where HF imports are downloaded (for the cleanup rows). */
    QString downloadsDir() const;
    /** Drag-and-drop order for the HF queue; target active = start now. */
    bool moveHfTaskBefore(const QString &draggedId, const QString &targetId);
    bool prioritizeHfTask(const QString &id);

signals:
    void installedChanged();
    /** HuggingFace download/import activity: pending tasks (running + queued). */
    void hfActivityChanged(int pending);

private slots:
    void onSearch();
    void onItemSelected(const QString &itemId);
    void onAction(const QString &itemId, const QString &variantId);
    void onCancel();
    void onCancelVariant(const QString &itemId, const QString &variantId);
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished(bool ok, const QString &path, const QString &error);
    void onPullProgress(const QString &label, int percent,
                        qint64 received, qint64 total);
    void onPullFinished(OllamaJob *job, const QString &itemId, const QString &name,
                        bool ok, const QString &output);
    void onCreateFinished(const QString &name, bool ok, const QString &output, quint64 freed);

private:
    void setupUi();
    void rebuildItems();
    void openFilters();
    void ensureHfToken();
    /** 401 from the Hub: append the "use the Token… button" hint. */
    QString hfErrorText(const QString &error) const;
    void updateFilterButton();
    void advanceHfQueue();
    void notifyHfActivity();
    /** Cancelled import: drop its unfinished shards instead of leaving them. */
    void removeHfPartialSources();
    QString hfQueuePath() const;
    void saveHfQueue() const;
    void loadHfQueue();
    bool m_hfResumePending = false;
    // Source files of the import currently in `ollama create`: never deletable.
    QStringList m_hfActiveSources;
    void writeHfSidecar(const QString &sidecar, const QString &repo,
                        const HfVariant &variant) const;
    /** Cache of `ollama list`, fed by whoever ran it (no CLI call here). */
    void applyInstalledList(const QList<OllamaCli::ModelInfo> &models);
    void applySort(QList<RegistryClient::Model> &models) const;
    QString hfSortKey() const;

    // Install/update marks for the variant picker. The panel itself only knows
    // the state flags; the source decides how to establish them.
    /** Push the stored flags of every Ollama variant of `itemId` into the panel. */
    void applyOllamaState(const QString &itemId, const QString &ref);
    /** Queue manifest checks for the installed tags (one request at a time). */
    void checkOllamaUpdates(const QString &itemId, const QStringList &refs);
    void startNextUpdateCheck();
    /** Push the stored flags of every HF variant of `repo` into the panel. */
    void applyHfStates(const QString &repo);
    /** Ask for the current commit of `repo` once per session. */
    void ensureRepoInfo(const QString &repo);
    /**
     * Is one repository variant installed? Provenance is the authority: a model
     * may have been renamed (or created under an older naming scheme), so the
     * generated name alone is not an identity. On success `installedSha`
     * receives the commit the local copy came from.
     */
    bool hfVariantInstalled(const QString &repo, const QString &firstFile,
                            const QString &localName, QString *installedSha) const;

    bool isOllama() const { return m_source == Source::Ollama; }

    // HuggingFace download queue: a split quant arrives as N files, downloaded
    // one after another into the same folder so the Modelfile can point at a
    // wildcard over the original shard names.
    void startHfDownload(const QString &repo, const HfVariant &variant);
    void startNextPart();
    void finishHfDownload();
    void stopHfQueue();
    /** Rebuild the picker from m_hfVariants (installed marks, new language). */
    void applyHfVariants(const QString &itemId);

    Source m_source;
    InstallService *m_install;
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
    QList<HfVariant> m_hfVariants;  // variants of m_pendingRepo, for the queue
    ModelFilters m_filters;
    QSet<QString> m_installedNames;
    QHash<QString, QString> m_localIds;        // installed name -> `ollama list` id
    QHash<QString, QString> m_manifestDigests; // ollama ref -> registry digest (session)
    QStringList m_updateQueue;                 // refs whose manifest is still to check
    QString m_updateItemId;                    // family those refs belong to
    QHash<QString, QString> m_repoShas;        // hf repo -> current commit (session)
    QSet<QString> m_repoInfoRequested;         // repo info already asked for
    QString m_pendingItemId;
    QString m_pendingRepo;
    QString m_pendingFile;
    // This panel's own operations: results of other tabs never reach it.
    QPointer<OllamaJob> m_pullJob;
    QString m_pullItemId;
    QString m_pullVariantId;
    // Further pulls from this tab wait in OllamaCli's queue; only the running
    // one drives the progress bar, the rest show "в очереди" in the status.
    struct QueuedPull {
        QPointer<OllamaJob> job;
        QString itemId;
        QString variantId;
    };
    QList<QueuedPull> m_queuedPulls;
    QSet<QString> m_pullVariants;  // variants already running/queued here

    // HuggingFace imports queue behind the running one (download + create).
    struct HfPending {
        QString repo;
        QString itemId;
        HfVariant variant;
    };
    QList<HfPending> m_hfQueue;
    QString m_activeHfItemId;
    QString m_activeHfVariantId;
    // Preemption by drag-and-drop: the expected cancellation of the running
    // reply is ignored, the promoted task is started directly (no control flow
    // through HfClient's finished() signal, which could re-enter the client).
    bool m_hfIgnoreCancel = false;
    HfVariant m_activeHfVariant;  // enough to requeue a preempted import
    // Set while `ollama create` runs for a freshly downloaded GGUF: the model
    // is not "installed" until that job finishes successfully.
    QPointer<OllamaJob> m_createJob;
    bool m_tokenLoaded = false;    // wallet read already attempted

    QString m_queueRevision;       // commit the queue downloads from ("" = main)
    QStringList m_queueFiles;      // parts still to download, part order
    QList<qint64> m_queueSizes;    // planned bytes per part (0 when unknown)
    QString m_queueFirstFile;
    QString m_queueLastPath;       // path of the last completed part
    int m_queueMainCount = 0;      // shards of the model itself (projector follows)
    int m_queueIndex = 0;
    qint64 m_queueDoneBytes = 0;
    qint64 m_queueTotalBytes = 0;
    qint64 m_queuePartTotal = 0;   // total reported for the current part
    bool m_queueActive = false;
    bool m_cancelRequested = false;
    bool m_loaded = false;
};

#endif // DISCOVERPANEL_H
