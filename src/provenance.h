#ifndef PROVENANCE_H
#define PROVENANCE_H

#include <QJsonObject>
#include <QObject>
#include <QString>

// Remembers where an installed model came from, so the "On device" list can
// tell the user which source to ask about updates.
//
// Stored as JSON in ~/.config/ollama-manager/models.json (apppaths.h); a file
// left at the pre-2026-11 location is moved there on first use. Entries:
//   {
//     "qwen3:8b": { "source": "ollama", "ref": "qwen3:8b", "digest": "abc123…" },
//     "repo/file.gguf": { "source": "hf", "ref": "owner/repo", "file": "x.gguf",
//                          "sha": "0123…", "installedAt": "2026-11-21T12:00:00Z" }
//   }
//
// The application shares one store (instance()); services get it injected
// (InstallService), tests build their own on a temporary path (A-3). Every
// change is announced with changed(), so views do not have to re-read the
// store by hand after each operation.
class Provenance : public QObject
{
    Q_OBJECT
public:
    struct Entry {
        QString source;       // "ollama", "hf" or empty
        QString ref;          // ollama: family:tag, hf: owner/repo
        QString file;         // hf: file name inside the repo
        QString digest;       // ollama: manifest digest (short) at install time
        QString sha;          // hf: repository commit sha at download time
        QString installedAt;  // ISO-8601 UTC

        bool isEmpty() const { return source.isEmpty(); }
    };

    static Provenance &instance();

    /** A store on `path` (no legacy migration): for tests and tools. */
    explicit Provenance(const QString &path, QObject *parent = nullptr);

    void record(const QString &modelName, const Entry &entry);
    Entry lookup(const QString &modelName) const;
    bool contains(const QString &modelName) const;
    void remove(const QString &modelName);

    /**
     * Moves the entry from oldName to newName, keeping every field (ref, file,
     * digest, sha, installedAt) untouched. Call after `ollama cp`/`rm` succeeded:
     * the original ref must survive a rename so the other quants of the same
     * repository can still be found.
     */
    void rename(const QString &oldName, const QString &newName);

    /**
     * Rewrites every string field that starts with the directory `from` to
     * start with `to` instead (the download directory moved). Returns the
     * number of rewritten fields. The current format stores repository file
     * names only; this keeps hand-edited or future absolute paths valid.
     * All or nothing: when the file cannot be written, the in-memory copy is
     * rolled back and -1 is returned (the disk still points at `from`).
     */
    int rebasePaths(const QString &from, const QString &to);

    /** Path of the JSON file this store reads and writes. */
    QString path() const { return m_path; }

    /** Where the file lives (exposed for the self-test). */
    static QString storePath();
    /** Where older versions kept it (AppConfigLocation/models.json). */
    static QString legacyStorePath();
    /**
     * Move `from` to `to` unless `to` already exists (copy if rename fails).
     * Returns true when a file was migrated. Public for the self-test.
     */
    static bool migrateLegacy(const QString &from, const QString &to);

signals:
    /** An entry was recorded, removed or renamed (both names are announced on rename). */
    void changed(const QString &modelName);

private:
    Provenance();
    void load();
    /** Writes the store; false when the file could not be written (rolled back by the caller). */
    bool save() const;

    QString m_path;
    QJsonObject m_data;
};

#endif // PROVENANCE_H
