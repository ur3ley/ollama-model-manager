#ifndef GGUFMETA_H
#define GGUFMETA_H

#include <QString>

// Minimal, read-only GGUF metadata reader.
//
// Only the header and the metadata key/value section are parsed (plus the
// tensor-info table, to count parameters); tensor data is never touched, so
// inspecting a 60 GB file costs a few seeks, not gigabytes of I/O.
//
// The class exists because Ollama happily creates a model from a GGUF whose
// embedded chat template belongs to a different model family. A "thinking"
// Qwen3 Go template planted on top of a non-thinking Instruct GGUF makes
// Ollama append " /think" to the user message, and the model then echoes that
// marker into its answer. Catching such templates before `ollama create`
// saves an 18 GB round trip.
struct GgufMeta
{
    bool ok = false;
    QString error;

    quint32 version = 0;
    quint64 tensorCount = 0;
    quint64 kvCount = 0;
    quint64 parameterCount = 0;

    QString architecture;
    QString name;
    QString quantization;
    int contextLength = 0;

    // Recommended sampler parameters (`general.sampling.*`), -1 when absent.
    double samplingTemperature = -1;
    double samplingTopP = -1;
    int samplingTopK = -1;

    bool hasChatTemplate = false;
    QString chatTemplate;

    // Template diagnostics
    bool templateHasSoftSwitch = false;  // IsThinkSet / "/think" / "/no_think"
    bool templateHasThinkTags = false;   // <think> ... </think>

    // Read metadata from a GGUF file. On failure `ok` is false and `error`
    // holds a human-readable reason (English, translated by the caller).
    static GgufMeta read(const QString &path);

    // Human-readable name for general.file_type (llama.cpp enum).
    static QString quantizationName(quint32 fileType);
};

#endif // GGUFMETA_H
