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
        double temperature = 0.7;

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

    static QString build(const Config &config);
    static Config defaultConfig();
};

#endif // MODEFILEBUILDER_H
