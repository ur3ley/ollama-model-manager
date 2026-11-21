#ifndef REPOGROUP_H
#define REPOGROUP_H

#include <QList>
#include <QString>
#include <QStringList>

#include "ollamacli.h"

class Provenance;

// An installed model as seen from the "Installed" tab: one quant of a repository.
struct InstalledQuant {
    QString modelName;     // current name in Ollama (what every client sees)
    QString originalRef;   // provenance.ref (family:tag / owner/repo) or modelName
    QString originalLabel; // original model name: qwen3.8:latest, <repo>:iq4_xs-mtp
    QString quantLabel;    // hf: IQ4_XS-MTP; empty for the Ollama registry
    QString file;          // hf: file name inside the repository
    QString digest;       // ModelInfo.id
    QString size;         // as printed by `ollama list`
    QString modified;

    bool renamed() const
    {
        const QString original = originalLabel.isEmpty() ? originalRef : originalLabel;
        return !original.isEmpty() && modelName != original;
    }
};

// One card in the "Installed" list. A repository may hold several installed
// quants (qwen3.8:latest + qwen3.8:q4_K_M, or several GGUF files of one HF repo);
// they are grouped by the original reference, never by the current name, so a
// rename does not split the card or lose where the model came from.
struct RepoGroup {
    QString id;       // "ollama:qwen3.8" | "hf:owner/repo" | "local:<name>"
    QString source;   // "ollama" | "hf" | "" (local)
    QString title;    // qwen3.8 | Qwen3.8-27B-GGUF | <model name>
    QString owner;    // ollama.com | unsloth | "" (local)
    QString ref;      // full provenance.ref of the first quant seen
    bool local = false;
    QList<InstalledQuant> quants;

    /** Everything the list filter matches: title, owner, refs and current names. */
    QString searchText() const;
    /** provenance.ref of an installed quant, or an empty string. */
    QString originalRefOf(const QString &modelName) const;
};

QList<RepoGroup> groupInstalledModels(const QList<OllamaCli::ModelInfo> &models,
                                      const Provenance &prov);

#endif // REPOGROUP_H
