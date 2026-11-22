#ifndef INSTALLSERVICE_H
#define INSTALLSERVICE_H

#include <QObject>
#include <QString>
#include <QStringList>

#include "provenance.h"

class OllamaCli;
class OllamaJob;

/**
 * Model installation and removal without widgets (A-2).
 *
 * The flows "pull → record where it came from", "downloaded GGUF → ollama
 * create → record → delete the downloaded copy", "delete → forget the record
 * and the leftover sources" and "rename → move the record" used to live in
 * the discover panels and the main window. Here they are one call each; the
 * returned OllamaJob is the caller's handle for progress and the result. The
 * bookkeeping is a finish hook of the job (OllamaJob::setFinishHook), so it
 * always runs before the caller's finished() slot — the order the connections
 * were made in cannot change the result the caller reads.
 */
class InstallService : public QObject
{
    Q_OBJECT
public:
    /** Dynamic property of an importHf() job: bytes of downloaded sources removed (qulonglong). */
    static constexpr const char *kFreedBytes = "freedBytes";

    InstallService(OllamaCli *cli, Provenance *provenance, QObject *parent = nullptr);

    OllamaCli *cli() const { return m_cli; }
    Provenance *provenance() const { return m_provenance; }

    /** `ollama pull`; on success the model is recorded as coming from the registry. */
    OllamaJob *pullFromRegistry(const QString &name, qint64 expectedBytes = 0);

    struct HfImport {
        QString name;          // the Ollama model to create
        QString modelfile;     // FROM lines pointing at the downloaded files
        QString repo;          // owner/repo
        QString file;          // first file of the variant, for provenance
        QString sha;           // repository commit at download time ("" unknown)
        QStringList sources;   // downloaded files, removed after a successful create
    };
    /** `ollama create` from downloaded GGUFs; on success record provenance and drop the sources. */
    OllamaJob *importHf(const HfImport &request);

    /** Plain `ollama create` (editor, GGUF import dialog). */
    OllamaJob *createModel(const QString &name, const QString &modelfile);

    /** `ollama rm`; on success forget the provenance entry and delete leftover downloads. */
    OllamaJob *deleteModel(const QString &name);

    /** Rename; on success — or a partial one, where the copy exists — move the provenance entry. */
    OllamaJob *renameModel(const QString &oldName, const QString &newName);

    /** Downloaded files (in `dir`) that belong to the provenance entry of a model. */
    static QStringList sourceFiles(const Provenance::Entry &entry, const QString &dir);
    /** `model-IQ4_XS-00001-of-00003.gguf` → `model-IQ4_XS-?????-of-00003.gguf`; a plain file matches itself. */
    static QString sourceFilePattern(const QString &file);
    /** Removes the files and their `.part` siblings; returns the bytes freed. */
    static quint64 removeSources(const QStringList &paths);

private:
    OllamaCli *m_cli;
    Provenance *m_provenance;
};

#endif // INSTALLSERVICE_H
