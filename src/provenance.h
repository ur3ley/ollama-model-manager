#ifndef PROVENANCE_H
#define PROVENANCE_H

#include <QJsonObject>
#include <QString>

// Remembers where an installed model came from, so the "On device" list can
// tell the user which source to ask about updates.
//
// Stored as JSON in the app config directory (models.json). Entries:
//   {
//     "qwen3:8b": { "source": "ollama", "ref": "qwen3:8b", "digest": "abc123…" },
//     "repo/file.gguf": { "source": "hf", "ref": "owner/repo", "file": "x.gguf",
//                          "sha": "0123…", "installedAt": "2026-11-21T12:00:00Z" }
//   }
class Provenance
{
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

    void record(const QString &modelName, const Entry &entry);
    Entry lookup(const QString &modelName) const;
    bool contains(const QString &modelName) const;
    void remove(const QString &modelName);

    /** Where the file lives (exposed for the self-test). */
    static QString storePath();

private:
    Provenance();
    void load();
    void save() const;

    QJsonObject m_data;
};

#endif // PROVENANCE_H
