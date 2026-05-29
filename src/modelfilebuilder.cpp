#include "modelfilebuilder.h"

ModelfileBuilder::Config ModelfileBuilder::defaultConfig()
{
    Config c;
    c.from.clear();
    c.enableNumCtx = true;
    c.numCtx = 32768;
    return c;
}

QString ModelfileBuilder::build(const Config &config)
{
    QString result;

    result += "FROM " + config.from + "\n";

    if (config.enableNumCtx && config.numCtx > 0) {
        result += "PARAMETER num_ctx " + QString::number(config.numCtx) + "\n";
    }
    if (config.enableTemperature) {
        result += "PARAMETER temperature " + QString::number(config.temperature, 'f', 2) + "\n";
    }
    if (config.enableTopP) {
        result += "PARAMETER top_p " + QString::number(config.topP, 'f', 2) + "\n";
    }
    if (config.enableTopK) {
        result += "PARAMETER top_k " + QString::number(config.topK) + "\n";
    }
    if (config.enableRepeatPenalty) {
        result += "PARAMETER repeat_penalty " + QString::number(config.repeatPenalty, 'f', 2) + "\n";
    }
    if (config.enableRepeatLastN) {
        result += "PARAMETER repeat_last_n " + QString::number(config.repeatLastN) + "\n";
    }
    if (config.enablePresencePenalty) {
        result += "PARAMETER presence_penalty " + QString::number(config.presencePenalty, 'f', 2) + "\n";
    }
    if (config.enableFrequencyPenalty) {
        result += "PARAMETER frequency_penalty " + QString::number(config.frequencyPenalty, 'f', 2) + "\n";
    }
    if (config.enableReasoning) {
        result += "PARAMETER think " + config.reasonValue + "\n";
    }

    QMapIterator<QString, QString> it(config.extraParams);
    while (it.hasNext()) {
        it.next();
        result += "PARAMETER " + it.key() + " " + it.value() + "\n";
    }

    if (config.enableSystemPrompt && !config.systemPrompt.isEmpty()) {
        result += "SYSTEM \"\"\"\n";
        result += config.systemPrompt;
        result += "\n\"\"\"\n";
    }

    if (!config.templateStr.isEmpty()) {
        result += "TEMPLATE \"\"\"\n";
        result += config.templateStr;
        result += "\n\"\"\"\n";
    }

    return result;
}
