#include "modelfilebuilder.h"

ModelfileBuilder::Config ModelfileBuilder::defaultConfig()
{
    Config c;
    c.from.clear();
    c.enableNumCtx = true;
    c.numCtx = 32768;
    return c;
}

bool ModelfileBuilder::breaksTripleQuotes(const QString &value)
{
    return value.contains(QLatin1String("\"\"\""));
}

QString ModelfileBuilder::build(const Config &config, QString *error)
{
    const bool systemUsed = config.enableSystemPrompt && !config.systemPrompt.isEmpty();
    if (systemUsed && breaksTripleQuotes(config.systemPrompt)) {
        if (error)
            *error = QStringLiteral("SYSTEM");
        return QString();
    }
    if (breaksTripleQuotes(config.templateStr)) {
        if (error)
            *error = QStringLiteral("TEMPLATE");
        return QString();
    }
    if (error)
        error->clear();

    QString result;

    result += fromLine(config.from);

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

    QMapIterator<QString, QString> it(config.extraParams);
    while (it.hasNext()) {
        it.next();
        result += "PARAMETER " + it.key() + " " + it.value() + "\n";
    }

    if (systemUsed) {
        result += "SYSTEM \"\"\"\n";
        result += config.systemPrompt;
        result += "\n\"\"\"\n";
    }

    if (!config.templateStr.isEmpty()) {
        result += "TEMPLATE \"\"\"\n";
        result += config.templateStr;
        result += "\n\"\"\"\n";
    }

    if (!config.renderer.isEmpty())
        result += "RENDERER " + config.renderer + "\n";

    if (!config.parser.isEmpty())
        result += "PARSER " + config.parser + "\n";

    return result;
}

QString ModelfileBuilder::fromLine(const QString &value)
{
    bool hasSpace = false;
    for (qsizetype i = 0; i < value.size() && !hasSpace; ++i)
        hasSpace = value.at(i).isSpace();
    // Ollama has no escape inside "…": a path with a quote stays unquoted.
    if (hasSpace && !value.contains(QLatin1Char('"')))
        return QStringLiteral("FROM \"") + value + QStringLiteral("\"\n");
    return QStringLiteral("FROM ") + value + QLatin1Char('\n');
}
