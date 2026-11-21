#ifndef QUANTMATCH_H
#define QUANTMATCH_H

#include <QString>

// Small pure helpers shared by the editor and the source-binding dialog.
// They used to live inside modeleditorwidget.cpp; the binding logic needs the
// same rules, so they moved here and are covered by the offline self-test.

/** Tag/family part without a trailing quantization suffix:
 *  "8b-instruct-q4_k_m" → "8b-instruct", "latest" → "latest". */
QString stripQuantSuffix(const QString &tagPart);

/** Search stem suggested for "find the source of this model" queries:
 *  the part before ':' with the quantization suffix removed. */
QString bindSearchStem(const QString &modelName);

/**
 * True when a local `ollama list` id and a registry manifest digest describe
 * the same model. `sha256:` prefixes are ignored and a short local id (ollama
 * prints a truncated digest) matches its full registry counterpart.
 */
bool digestMatches(const QString &localId, const QString &registryDigest);

/** What the variant picker shows for one quantization / repository file. */
enum class VariantState {
    NotInstalled,     // no local copy: the action downloads it
    Installed,        // local copy matches the source: nothing to do
    UpdateAvailable,  // local copy is stale: the action pulls the new version
};

/**
 * State from the two facts the browsers can establish: the model is installed
 * and the local copy is identical to the source (same manifest digest or
 * commit sha). "Installed but not comparable" counts as Installed — a missing
 * source digest must never fake an update.
 */
VariantState variantState(bool installed, bool sameAsSource);

/**
 * Repository update check: true when both commits are known and name different
 * revisions. An empty installed sha (entries recorded before sha tracking)
 * means "no update", not "different": a mismatch has to be proven.
 */
bool shaDiffers(const QString &installedSha, const QString &remoteSha);

/**
 * File name of a GGUF without its path, its `.gguf` suffix and a split part
 * suffix: `Q4_K_M/model-Q4_K_M-00001-of-00003.gguf` → `model-Q4_K_M`.
 * Two files with the same stem are the same variant (a split model or a plain
 * file of the same quant).
 */
QString ggufFileNameStem(const QString &fileName);

/** Do two GGUF file names belong to the same variant group? Paths, case, the
 *  `.gguf` suffix and split part numbers are ignored. */
bool sameGgufVariant(const QString &left, const QString &right);

/**
 * Quantization label of a GGUF file name, including the suffix that tells two
 * otherwise identical quants apart: `…-IQ4_XS-MTP-00001-of-00002.gguf` →
 * `IQ4_XS-MTP`, `…-Q2_K.gguf` → `Q2_K`, `…-UD-Q4_K_XL.gguf` → `UD-Q4_K_XL`.
 * Empty when the name carries no recognizable quant.
 *
 * The same helper feeds the picker label and the generated Ollama tag, so the
 * row the user picks and the installed model name always agree.
 */
QString quantLabelFromName(const QString &fileName);

/**
 * Canonical Ollama name for a HuggingFace GGUF variant:
 * `unsloth/Qwen3.8-27B-GGUF` + `…-IQ4_XS-MTP-00001-of-00003.gguf` →
 * `qwen3.8-27b:iq4_xs-mtp`. Used both for the installed model name and for the
 * FROM label, so the two always agree.
 */
QString canonicalHfModelName(const QString &repo, const QString &fileName);

#endif // QUANTMATCH_H
