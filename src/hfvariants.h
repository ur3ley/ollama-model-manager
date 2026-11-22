#ifndef HFVARIANTS_H
#define HFVARIANTS_H

#include "hfclient.h"

#include <QList>
#include <QString>
#include <QStringList>

// One selectable quantization of a HuggingFace GGUF repository.
//
// A repository often publishes the same quant as several split files
// (`model-Q4_K_M-00001-of-00003.gguf`, …). The browser must show that as one
// entry, not as one entry per file: the raw file count for big repos runs into
// the hundreds and the variant picker becomes unusable.
struct HfVariant {
    QString id;        // first file of the group (or the single file)
    QString label;     // Q4_K_M, UD-Q4_K_XL, IQ4_XS-MTP, … (file name when no quant is known)
    QString quant;
    qint64 size = 0;   // total bytes, parts summed
    int parts = 0;     // N for a split GGUF, 0 otherwise
    bool split = false;
    bool projector = false;  // mmproj / vision projector, not an Ollama model
    bool draft = false;      // MTP draft model, used with a speculative decoder only
    // Split GGUFs are supported since Ollama accepts a wildcard
    // (`FROM model-*.gguf`), so only projectors are not installable.
    bool enabled = true;
    QStringList files;       // every file that belongs to the variant, part order
    QList<qint64> sizes;     // bytes per file, same order as `files`
};

// `model-Q4_K_M-00001-of-00003.gguf` -> `model-Q4_K_M-?????-of-00003.gguf`.
//
// Ollama imports a split GGUF from a wildcard over the original shard names,
// so the Modelfile needs the base name without the `-0000N-of-0000M` suffix.
// The directory is dropped because downloads are flattened into one folder.
QString splitWildcardPattern(const QString &firstFileName);

// Group raw repository files into one variant per quantization:
//  - split GGUF parts are merged (size summed, parts = N),
//  - projectors stay separate entries,
//  - duplicates with the same label and size are dropped,
//  - selectable single-file variants come first, smallest first; split and
//    projector entries go last (split ones are installable via a wildcard
//    Modelfile, projectors are shown only as an explanation).
QList<HfVariant> groupHfFiles(const QList<HfClient::File> &files);

#endif // HFVARIANTS_H
