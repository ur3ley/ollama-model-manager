#include "repogroup.h"

#include "provenance.h"
#include "quantmatch.h"

#include <QHash>

#include <algorithm>

namespace {

QString ollamaFamily(const QString &ref)
{
    return ref.section(QLatin1Char(':'), 0, 0);
}

} // namespace

QString RepoGroup::searchText() const
{
    QStringList parts;
    parts << title << owner << ref;
    for (const InstalledQuant &quant : quants) {
        parts << quant.modelName;
        if (quant.originalRef != quant.modelName)
            parts << quant.originalRef;
        if (!quant.originalLabel.isEmpty())
            parts << quant.originalLabel;
        if (!quant.quantLabel.isEmpty())
            parts << quant.quantLabel;
        if (!quant.file.isEmpty())
            parts << quant.file;
    }
    return parts.join(QLatin1Char(' '));
}

QString RepoGroup::originalRefOf(const QString &modelName) const
{
    for (const InstalledQuant &quant : quants) {
        if (quant.modelName == modelName)
            return quant.originalRef;
    }
    return QString();
}

QList<RepoGroup> groupInstalledModels(const QList<OllamaModelInfo> &models,
                                      const Provenance &prov)
{
    QList<RepoGroup> groups;
    QHash<QString, int> indexById;

    for (const OllamaModelInfo &model : models) {
        if (model.name.isEmpty())
            continue;

        const Provenance::Entry entry = prov.lookup(model.name);

        InstalledQuant quant;
        quant.modelName = model.name;
        quant.originalRef = entry.ref.isEmpty() ? model.name : entry.ref;
        quant.file = entry.file;
        if (!entry.file.isEmpty()) {
            // The variant's canonical Ollama name (repo stem + quant tag). It is
            // what FROM shows, so a renamed model still tells where it came from.
            const QString canonical = canonicalHfModelName(entry.ref, entry.file);
            quant.originalLabel = canonical.isEmpty() ? quant.originalRef : canonical;
            const QString label = quantLabelFromName(entry.file);
            quant.quantLabel = label.isEmpty() ? ggufFileNameStem(entry.file) : label;
        } else {
            quant.originalLabel = quant.originalRef;
        }
        quant.digest = model.id;
        quant.size = model.size;
        quant.modified = model.modified;

        RepoGroup group;
        if (entry.source == QLatin1String("ollama")) {
            group.source = QStringLiteral("ollama");
            group.ref = entry.ref.isEmpty() ? model.name : entry.ref;
            group.title = ollamaFamily(group.ref);
            group.owner = QStringLiteral("ollama.com");
            group.id = QStringLiteral("ollama:") + group.title;
        } else if (entry.source == QLatin1String("hf")) {
            group.source = QStringLiteral("hf");
            group.ref = entry.ref.isEmpty() ? model.name : entry.ref;
            const int slash = group.ref.indexOf(QLatin1Char('/'));
            if (slash > 0) {
                group.owner = group.ref.left(slash);
                group.title = group.ref.mid(slash + 1);
            } else {
                group.title = group.ref;
            }
            group.id = QStringLiteral("hf:") + group.ref;
        } else {
            // No provenance: a model created locally. It is its own repository.
            group.local = true;
            group.title = model.name;
            group.ref = entry.ref;
            group.id = QStringLiteral("local:") + model.name;
        }

        const int existing = indexById.value(group.id, -1);
        if (existing >= 0) {
            groups[existing].quants.append(quant);
            continue;
        }
        group.quants.append(quant);
        indexById.insert(group.id, groups.size());
        groups.append(group);
    }

    for (RepoGroup &group : groups) {
        std::sort(group.quants.begin(), group.quants.end(),
                  [](const InstalledQuant &a, const InstalledQuant &b) {
                      const int cmp = a.originalRef.compare(b.originalRef, Qt::CaseInsensitive);
                      if (cmp != 0)
                          return cmp < 0;
                      return a.modelName.compare(b.modelName, Qt::CaseInsensitive) < 0;
                  });
    }

    std::sort(groups.begin(), groups.end(), [](const RepoGroup &a, const RepoGroup &b) {
        const int cmp = a.title.compare(b.title, Qt::CaseInsensitive);
        if (cmp != 0)
            return cmp < 0;
        return a.id < b.id;
    });

    return groups;
}
