#ifndef OLLAMATYPES_H
#define OLLAMATYPES_H

#include <QMultiMap>
#include <QString>
#include <QStringList>

// Plain data about installed Ollama models. Kept apart from OllamaCli so that
// domain code (repogroup, filters) does not depend on the process wrapper (A-6).

/** One row of `ollama list`. */
struct OllamaModelInfo {
    QString name;
    QString id;
    QString size;
    QString modified;
};

/** What `ollama show` + `ollama show --modelfile` report about a model. */
struct OllamaModelParams {
    QString architecture;
    QString paramCount;
    int contextLength = 0;
    QString quantization;
    // A Modelfile may repeat a parameter (several `stop` lines): a multi-map
    // keeps them all instead of folding them into the last one (B-7).
    QMultiMap<QString, QString> parameters;
    QString systemPrompt;
    QString templateStr;
    QString renderer;
    QString parser;
    QString fromBase;
    QStringList capabilities;
};

#endif // OLLAMATYPES_H
