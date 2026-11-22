#ifndef MODEFILEBUILDER_H
#define MODEFILEBUILDER_H

#include <QString>
#include <QMap>

class ModelfileBuilder
{
public:
    struct Config {
        QString from;

        bool enableNumCtx = true;
        int numCtx = 32768;

        bool enableTemperature = false;
        double temperature = 0.8;

        bool enableTopP = false;
        double topP = 0.9;

        bool enableTopK = false;
        int topK = 40;

        bool enableRepeatPenalty = false;
        double repeatPenalty = 1.1;

        bool enableRepeatLastN = false;
        int repeatLastN = 64;

        bool enablePresencePenalty = false;
        double presencePenalty = 0.0;

        bool enableFrequencyPenalty = false;
        double frequencyPenalty = 0.0;

        bool enableSystemPrompt = false;
        QString systemPrompt;

        QString templateStr;

        // Ollama >= 0.12: RENDERER / PARSER select the prompt renderer and the
        // output parser without pasting a raw TEMPLATE. This is the sanctioned
        // way to override a model whose embedded template is wrong.
        QString renderer;
        QString parser;

        QMap<QString, QString> extraParams;
    };

    /**
     * Renders the Modelfile. Multi-line values go into `"""…"""` blocks,
     * which have no escape syntax: a value that itself contains `"""` would
     * end the block early and turn its tail into Modelfile commands. Such a
     * config is refused — the result is empty and `error` (if given) names the
     * offending directive.
     */
    static QString build(const Config &config, QString *error = nullptr);
    /** True when `value` cannot be written as a triple-quoted Modelfile block. */
    static bool breaksTripleQuotes(const QString &value);
    /**
     * `FROM <value>\n`. A value with whitespace (a GGUF path under a directory
     * with spaces) is put in double quotes, which the Ollama Modelfile parser
     * strips; without quotes it would end at the first space.
     */
    static QString fromLine(const QString &value);
    static Config defaultConfig();
};

#endif // MODEFILEBUILDER_H
