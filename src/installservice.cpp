#include "installservice.h"

#include "apppaths.h"
#include "hfvariants.h"
#include "ollamacli.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

InstallService::InstallService(OllamaCli *cli, Provenance *provenance, QObject *parent)
    : QObject(parent)
    , m_cli(cli)
    , m_provenance(provenance)
{
}

OllamaJob *InstallService::pullFromRegistry(const QString &name, qint64 expectedBytes)
{
    OllamaJob *job = m_cli->pullModelAsync(name, expectedBytes);
    Provenance *provenance = m_provenance;
    job->setFinishHook([provenance, name](OllamaJob *finishedJob) {
        if (!finishedJob->success())
            return;
        Provenance::Entry entry;
        entry.source = QStringLiteral("ollama");
        entry.ref = name;
        provenance->record(name, entry);
    });
    return job;
}

OllamaJob *InstallService::importHf(const HfImport &request)
{
    OllamaJob *job = m_cli->createModel(request.name, request.modelfile);
    Provenance *provenance = m_provenance;
    job->setFinishHook([provenance, request](OllamaJob *finishedJob) {
        // A failed create keeps the sources: a retry must not download them again.
        if (!finishedJob->success())
            return;
        Provenance::Entry entry;
        entry.source = QStringLiteral("hf");
        entry.ref = request.repo;
        entry.file = request.file;
        // The commit the file came from is the baseline of the next update
        // check; unknown (repo info failed) leaves the field empty, which the
        // comparison treats as "no update".
        entry.sha = request.sha;
        provenance->record(request.name, entry);
        // Ollama has copied the shards into its blob store: the downloaded
        // sources are now a second full copy.
        finishedJob->setProperty(kFreedBytes, qulonglong(removeSources(request.sources)));
    });
    return job;
}

OllamaJob *InstallService::createModel(const QString &name, const QString &modelfile)
{
    return m_cli->createModel(name, modelfile);
}

OllamaJob *InstallService::deleteModel(const QString &name)
{
    // Captured before the delete: once the model is gone the entry is the only
    // link to its downloaded sources.
    const QStringList sources = sourceFiles(m_provenance->lookup(name), apppaths::downloadDir());
    OllamaJob *job = m_cli->deleteModelAsync(name);
    Provenance *provenance = m_provenance;
    job->setFinishHook([provenance, name, sources](OllamaJob *finishedJob) {
        if (!finishedJob->success())
            return;
        // Otherwise the browser keeps claiming the quant is "downloaded".
        provenance->remove(name);
        removeSources(sources);
    });
    return job;
}

OllamaJob *InstallService::renameModel(const QString &oldName, const QString &newName)
{
    OllamaJob *job = m_cli->renameModelAsync(oldName, newName);
    Provenance *provenance = m_provenance;
    job->setFinishHook([provenance, oldName, newName](OllamaJob *finishedJob) {
        const bool ok = finishedJob->success();
        // The original ref is what ties the model to its repository: it moves
        // with the copy even when the old name could not be removed (B-5).
        if (ok || finishedJob->partial())
            provenance->rename(oldName, newName);
    });
    return job;
}

QString InstallService::sourceFilePattern(const QString &file)
{
    // The same glob the Modelfile uses: only the shards of this series (B-13).
    return splitWildcardPattern(file);
}

QStringList InstallService::sourceFiles(const Provenance::Entry &entry, const QString &dir)
{
    QStringList result;
    if (entry.file.isEmpty())
        return result;
    const QDir directory(dir);
    const QStringList names = directory.entryList({sourceFilePattern(entry.file)}, QDir::Files);
    for (const QString &name : names)
        result << directory.filePath(name);
    return result;
}

quint64 InstallService::removeSources(const QStringList &paths)
{
    quint64 freed = 0;
    for (const QString &path : paths) {
        const qint64 size = QFileInfo(path).size();
        if (QFile::remove(path))
            freed += quint64(qMax<qint64>(size, 0));
        QFile::remove(path + QStringLiteral(".part"));
        QFile::remove(path + QStringLiteral(".part.rev"));
    }
    return freed;
}
