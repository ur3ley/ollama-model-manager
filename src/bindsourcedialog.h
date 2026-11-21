#ifndef BINDSOURCEDIALOG_H
#define BINDSOURCEDIALOG_H

#include <QDialog>
#include <QList>
#include <QObject>
#include <QString>

#include "hfclient.h"
#include "registryclient.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QProgressBar;
class QPushButton;
class QTabWidget;

/** One installed model to look a source up for. */
struct BindTarget {
    QString modelName;  // current name in Ollama
    QString localId;    // ModelInfo.id (manifest digest)
};

/**
 * Restores provenance for models installed before it was tracked.
 *
 * Runs the Ollama registry search ("which tag has my digest?") without any UI
 * so both the dialog and the batch tool in "Tools" share one implementation.
 * Every match is written to Provenance here; the entry's installedAt is kept
 * when the model already had one, so re-running the search never rewrites it.
 */
class SourceAutoBinder : public QObject
{
    Q_OBJECT
public:
    explicit SourceAutoBinder(QObject *parent = nullptr);

    void setTargets(const QList<BindTarget> &targets);
    void start();
    void cancel();

    bool isRunning() const { return m_running; }
    QString currentModel() const;
    int checked() const { return m_checked; }
    int total() const { return m_total; }

signals:
    /** Tags examined for the current model: "checking N of M". */
    void progress(const QString &modelName, int checked, int total);
    void bound(const QString &modelName, const QString &ref, const QString &digest);
    void targetFailed(const QString &modelName);
    void finished(int boundCount, int targetCount);

private slots:
    void onTags(bool ok, const QList<RegistryClient::Tag> &tags, const QString &error);
    void onManifest(const QString &ref, const QString &digest, bool ok, const QString &error);

private:
    void nextModel();
    void nextFamily();
    void nextTag();
    void finishTarget(bool success);

    RegistryClient *m_registry;
    QList<BindTarget> m_targets;
    int m_index = -1;
    QStringList m_families;
    QList<RegistryClient::Tag> m_tags;
    int m_tagIndex = 0;
    int m_checked = 0;
    int m_total = 0;
    int m_boundCount = 0;
    bool m_running = false;
    bool m_cancelled = false;
};

/**
 * «Привязать источник…»: restore the origin of an installed model without
 * reinstalling it. Two tabs — Ollama (registry tags matched by manifest digest,
 * plus an automatic pass) and HuggingFace (GGUF repo + file + current sha).
 */
class BindSourceDialog : public QDialog
{
    Q_OBJECT
public:
    BindSourceDialog(const QString &modelName, const QString &localId,
                     QWidget *parent = nullptr);

signals:
    /** Provenance was written: the list/editor should reload this model. */
    void bound(const QString &modelName);

private slots:
    void onOllamaSearch();
    void onHfSearch();
    void onTabChanged(int index);

    void onOllamaTags(bool ok, const QList<RegistryClient::Tag> &tags, const QString &error);
    void onOllamaModelActivated();
    void onBindOllamaTag();

    void onHfSearchFinished(bool ok, const QList<HfClient::Repo> &repos, const QString &error);
    void onHfRepoSelected();
    void onHfFiles(bool ok, const QList<HfClient::File> &files, const QString &error);
    void onHfRepoInfo(const QString &repo, const QString &sha, bool ok, const QString &error);
    void onBindHfFile();

    void onAutoDetect();
    void onAutoProgress(const QString &modelName, int checked, int total);
    void onAutoBound(const QString &modelName, const QString &ref, const QString &digest);
    void onAutoFinished(int boundCount, int targetCount);

private:
    void setupUi();
    void retranslateUi();
    void startOllamaSearch();
    void startHfSearch();
    /** Writes provenance, keeping an existing installedAt; emits bound(). */
    void writeEntry(const QString &source, const QString &ref, const QString &file,
                    const QString &digest, const QString &sha);
    void setAutoRunning(bool running);
    QString preferredHfFile(const QList<HfClient::File> &files) const;

    QString m_modelName;
    QString m_localId;

    RegistryClient *m_registry;
    HfClient *m_hf;
    SourceAutoBinder *m_binder = nullptr;

    QLabel *m_modelLabel;
    QLabel *m_statusLabel;
    QTabWidget *m_tabs;

    // Ollama tab
    QPushButton *m_autoBtn;
    QPushButton *m_autoCancelBtn;
    QLabel *m_autoLabel;
    QProgressBar *m_autoBar;
    QLineEdit *m_ollamaSearchEdit;
    QPushButton *m_ollamaSearchBtn;
    QLabel *m_ollamaResultsLabel;
    QLabel *m_ollamaTagsLabel;
    QListWidget *m_ollamaModels;
    QListWidget *m_ollamaTags;
    QPushButton *m_ollamaBindBtn;

    // HuggingFace tab
    QLineEdit *m_hfSearchEdit;
    QPushButton *m_hfSearchBtn;
    QLabel *m_hfReposLabel;
    QLabel *m_hfFilesLabel;
    QListWidget *m_hfRepos;
    QListWidget *m_hfFiles;
    QPushButton *m_hfBindBtn;

    QList<RegistryClient::Tag> m_tags;
    QStringList m_hfFileNames;
    QString m_hfRepo;
    QString m_hfSha;
    QString m_pendingHfFile;
    bool m_ollamaSearched = false;
    bool m_hfSearched = false;
    bool m_hfBindPending = false;
};

#endif // BINDSOURCEDIALOG_H
