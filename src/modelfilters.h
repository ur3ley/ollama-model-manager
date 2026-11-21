#ifndef MODELFILTERS_H
#define MODELFILTERS_H

#include <QString>
#include <QStringList>

// Hub filters (a pragmatic subset of the Unsloth panel): capabilities,
// maximum parameter count and uploader. Pure functions keep the logic testable
// without the GUI.
struct ModelFilters {
    QStringList capabilities;  // required, lowercase: tools/thinking/vision/audio/embedding/...
    int maxParamsB = 0;        // 0 = any
    QString uploader;          // substring of the owner name, empty = any

    bool isActive() const
    {
        return !capabilities.isEmpty() || maxParamsB > 0 || !uploader.trimmed().isEmpty();
    }
};

namespace filterlogic {

/** All `required` capabilities are present in `caps` (case-insensitive). */
bool hasCapabilities(const QStringList &caps, const QStringList &required);

/** Largest "235b"-style size in the list, in billions; 0 when unknown. */
int largestSizeB(const QStringList &sizes);

/** True when the model has at least one size not exceeding `maxB`. */
bool fitsMaxParams(const QStringList &sizes, int maxB);

/** Maps HuggingFace tags to hub capability names. */
QStringList hfTagsToCapabilities(const QStringList &tags);

} // namespace filterlogic

#endif // MODELFILTERS_H
