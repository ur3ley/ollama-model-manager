#ifndef OLLAMACLI_H
#define OLLAMACLI_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>

#include "ollamatypes.h"
#include "pullprogress.h"

class OllamaCli;
class QTemporaryFile;

/**
 * One `ollama` operation (A-1).
 *
 * Every *Async() call of OllamaCli returns its own job: the caller connects to
 * that job's progress()/finished() and never has to guess from a model name
 * whether a broadcast result is its own. The job starts on the next event
 * loop iteration, so connecting right after the call never misses a result,
 * and finished() is emitted exactly once — also for a failed start, a crash,
 * an invalid name or a cancelled queued pull. The job deletes itself (later)
 * after finished(); hold it in a QPointer.
 */
class OllamaJob : public QObject
{
    Q_OBJECT
public:
    enum class Kind { Show, Create, Delete, Copy, Rename, Pull, BlobUsage };
    enum class State { Queued, Running, Finished };

    quint64 id() const { return m_id; }
    Kind kind() const { return m_kind; }
    /** The model the job is about: the target of create/delete/pull/show, the new name of copy/rename. */
    QString name() const { return m_name; }
    /** Copy/rename: the old name; empty otherwise. */
    QString sourceName() const { return m_source; }
    State state() const { return m_state; }
    bool isFinished() const { return m_state == State::Finished; }
    bool success() const { return m_success; }
    QString output() const { return m_output; }

    /** Rename only: the copy exists, but the old name could not be removed (B-5). */
    bool partial() const { return m_partial; }
    /** Show only: the parsed model details (empty when !success()). */
    OllamaModelParams params() const { return m_params; }
    /** BlobUsage only: the model's `FROM` blob and how many installed models use it (itself included). */
    QString blob() const { return m_blob; }
    int blobReferences() const { return m_blobRefs; }

    /** Pull: kill the download, or drop it from the queue. Other kinds run to the end. */
    void cancel();

    /**
     * Bookkeeping that must be done before anyone is told the job finished.
     *
     * InstallService records provenance and deletes the downloaded sources on
     * success, and the caller's finished() slot reads that result (the freed
     * bytes on the job, a record in models.json). A plain connect() would make
     * the outcome depend on which side was connected first; the hook always
     * runs inside OllamaCli::finish(), before finished() is emitted.
     */
    void setFinishHook(std::function<void(OllamaJob *job)> hook);

signals:
    /** Pull only. percent is -1 while the total is unknown. */
    void progress(const QString &label, int percent, qint64 received, qint64 total);
    void finished(bool success, const QString &output);

private:
    friend class OllamaCli;
    OllamaJob(OllamaCli *cli, quint64 id, Kind kind, const QString &name);

    OllamaCli *m_cli;
    quint64 m_id;
    Kind m_kind;
    QString m_name;
    QString m_source;
    State m_state = State::Queued;
    bool m_success = false;
    bool m_partial = false;
    bool m_cancelled = false;
    QString m_output;
    OllamaModelParams m_params;
    QString m_blob;
    int m_blobRefs = 0;

    QProcess *m_process = nullptr;  // the running step, owned by the job

    // Pull state: every job has its own parser, so a second pull can never
    // feed its output into the first one's buffer (B-2).
    qint64 m_expectedBytes = 0;
    QString m_pullBuffer;
    PullProgressParser m_pullParser;
    QString m_lastLabel;
    qint64 m_lastReceived = -1;
    int m_lastPercent = -2;
    // Set by prioritizePull(): the process is killed on purpose and the job
    // goes back to the queue without a finished() signal or a failure state.
    bool m_requeue = false;
    QString m_lastLine;  // last non-empty CLI line: the error text of a failed pull

    // BlobUsage state: the models still to inspect.
    QList<OllamaModelInfo> m_pendingModels;

    // Create from content: the private Modelfile lives as long as the job (S-1).
    QTemporaryFile *m_modelfile = nullptr;

    // Runs once in finish(), before finished(); set by the job's owner.
    std::function<void(OllamaJob *)> m_finishHook;
};

class OllamaCli : public QObject
{
    Q_OBJECT
public:
    using ModelInfo = OllamaModelInfo;
    using ModelParams = OllamaModelParams;

    /** `ollamaPath` empty — look `ollama` up in PATH (tests pass a fake binary). */
    explicit OllamaCli(QObject *parent = nullptr, const QString &ollamaPath = QString());

    /** false when no `ollama` binary was found (B-12). */
    bool isAvailable() const;
    QString ollamaPath() const;

    /**
     * Ollama model names as the registry accepts them: starts with a letter
     * or digit, then letters, digits and `._:/-`. A name starting with `-`
     * would reach `ollama` as an option (S-6); every job refuses such names.
     */
    static bool isValidModelName(const QString &name);

    /**
     * Refreshes the installed list and emits modelsListed(). This is shared
     * state rather than an operation: a newer request silently replaces an
     * older one, and every view takes the latest list.
     */
    void listModelsAsync();

    /** `ollama show` + `ollama show --modelfile`; the result is job->params(). */
    OllamaJob *showModelAsync(const QString &name);
    /** Writes `modelfileContent` into a private temporary file (0600) and runs `ollama create`. */
    OllamaJob *createModel(const QString &name, const QString &modelfileContent);
    OllamaJob *deleteModelAsync(const QString &name);
    OllamaJob *copyModelAsync(const QString &oldName, const QString &newName);
    /** Copy to the new name, then remove the old one; see OllamaJob::partial(). */
    OllamaJob *renameModelAsync(const QString &oldName, const QString &newName);
    /**
     * Pull from the Ollama registry. Pulls run one at a time: a pull started
     * while another runs waits in a queue instead of killing it (B-2).
     * `expectedBytes` is the advertised download size (0 when unknown), the
     * denominator of the aggregate percentage.
     */
    OllamaJob *pullModelAsync(const QString &name, qint64 expectedBytes = 0);
    /**
     * How many installed models share `name`'s FROM blob (the delete dialog).
     * Runs `show --modelfile` per model, without blocking the UI; blobs are
     * cached by model id, so a second dialog costs one `ollama list` (P-1).
     */
    OllamaJob *blobUsageAsync(const QString &name);

    /** A pull runs or waits in the queue. */
    bool isPulling() const;

    /** One pending pull for the global downloads panel. */
    struct PullInfo {
        quint64 id = 0;
        QString name;
        bool active = false;
        int percent = -1;
    };
    /** Running pull first, then the waiting ones in start order. */
    QList<PullInfo> pullList() const;
    bool cancelPull(quint64 id);
    /** Move a queued pull by delta (-1 earlier, +1 later); active jobs stay put. */
    bool movePull(quint64 id, int delta);
    /** `-partial` blob files older than an hour (safe to delete, no live pull). */
    QStringList stalePartialBlobs() const;
    /** Insert a queued pull right before another queued pull. */
    bool movePullBefore(quint64 draggedId, quint64 targetId);
    /**
     * Start a queued pull now. A running pull is suspended (its `ollama pull`
     * is stopped and put right behind the promoted one); completed blobs are
     * kept, so it resumes without re-downloading whole layers.
     */
    bool prioritizePull(quint64 id);

    /**
     * Ollama's model store (the directory holding `blobs/`): $OLLAMA_MODELS,
     * else the root seen in a `FROM <blob>` of a shown model, else the first
     * of the usual locations that has `blobs/`. Empty when nothing matches.
     */
    QString modelsDir() const;
    /** `FROM /<root>/blobs/sha256-…` of a Modelfile, any root; empty if none. */
    static QString blobFromModelfile(const QString &modelfileContent);
    /** "/var/lib/ollama/blobs/sha256-ab" → "/var/lib/ollama". */
    static QString modelsDirFromBlob(const QString &blobPath);

    // Parsing helpers (public for the offline self-tests).
    static QString extractFromBase(const QString &modelfileContent);
    static QString extractDirective(const QString &modelfileContent, const QString &directive);
    /** Fills the Modelfile-derived fields (FROM, SYSTEM, TEMPLATE, RENDERER, PARSER). */
    static void applyModelfile(ModelParams &params, const QString &modelfileContent);
    static QList<ModelInfo> parseListOutput(const QString &output);
    static ModelParams parseShowOutput(const QString &output);

signals:
    void modelsListed(bool ok, const QList<OllamaCli::ModelInfo> &models, const QString &error);
    /**
     * Emitted after job->finished(), for views that react to any change of
     * the installed set (status bar, list refresh). Not for result matching.
     */
    void jobFinished(OllamaJob *job);
    /** Pull pipeline changed: how many pulls run now and how many wait. */
    void pullActivityChanged(int active, int queued);
    /** A pull moved from the queue to running (normal start or promotion). */
    void pullStarted(quint64 id);
    /** A running pull was pushed back into the queue (user moved it down). */
    void pullSuspended(quint64 id);

private:
    friend class OllamaJob;
    using StepDone = std::function<void(bool ok, const QString &stdOut, const QString &stdErr)>;

    OllamaJob *newJob(OllamaJob::Kind kind, const QString &name);
    /** Deferred start: the caller connects first. Refuses invalid names. */
    void schedule(OllamaJob *job, const QStringList &names, std::function<void()> start);
    /** One `ollama` process for `job` (A-5: the single place that wires QProcess). */
    void runStep(OllamaJob *job, const QStringList &args, const StepDone &done);
    void finish(OllamaJob *job, bool success, const QString &output);
    /** One place to report the pull pipeline to the global download counter. */
    void notifyPullActivity();

    void startNextPull();
    void startPull(OllamaJob *job);
    void handlePullOutput(OllamaJob *job);
    void cancelJob(OllamaJob *job);
    void inspectNextBlob(OllamaJob *job);
    void learnModelsDir(const QString &modelfileContent);

    QString m_ollamaPath;
    QString m_modelsDir;  // learned from a FROM <blob> line, see modelsDir()
    quint64 m_nextJobId = 1;
    QProcess *m_list = nullptr;
    QPointer<OllamaJob> m_activePull;
    QList<QPointer<OllamaJob>> m_pullQueue;
    QHash<QString, QString> m_blobById;  // `ollama list` id → FROM blob
};

#endif // OLLAMACLI_H
