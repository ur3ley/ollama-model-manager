#include "ollamacli.h"

#include <QRegularExpression>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QFileInfo>

OllamaCli::OllamaCli(QObject *parent)
    : QObject(parent)
{
    m_ollamaPath = QStandardPaths::findExecutable("ollama");
    if (m_ollamaPath.isEmpty()) {
        m_ollamaPath = "ollama";
    }
}

bool OllamaCli::isAvailable() const
{
    return !m_ollamaPath.isEmpty();
}

QString OllamaCli::ollamaPath() const
{
    return m_ollamaPath;
}

QString OllamaCli::runCommand(const QStringList &args, int timeoutMs, bool *ok)
{
    QProcess process;
    process.start(m_ollamaPath, args);
    if (!process.waitForFinished(timeoutMs)) {
        if (ok) *ok = false;
        return process.errorString();
    }
    if (ok) *ok = (process.exitCode() == 0);
    return process.readAllStandardOutput();
}

QList<OllamaCli::ModelInfo> OllamaCli::listModels()
{
    QString output = runCommand({"list"});
    return parseListOutput(output);
}

OllamaCli::ModelParams OllamaCli::showModel(const QString &name)
{
    bool ok = false;
    QString output = runCommand({"show", name}, 30000, &ok);
    if (!ok) {
        return ModelParams();
    }
    ModelParams params = parseShowOutput(output);

    QString modelfileContent = showModelfile(name);
    if (!modelfileContent.isEmpty()) {
        params.fromBase = extractFromBase(modelfileContent);
        params.templateStr = extractDirective(modelfileContent, QStringLiteral("TEMPLATE"));
        params.renderer = extractDirective(modelfileContent, QStringLiteral("RENDERER"));
        params.parser = extractDirective(modelfileContent, QStringLiteral("PARSER"));
    }

    return params;
}

QString OllamaCli::showModelfile(const QString &name)
{
    bool ok = false;
    QString output = runCommand({"show", name, "--modelfile"}, 30000, &ok);
    if (!ok) {
        return QString();
    }
    return output;
}

void OllamaCli::createModelAsync(const QString &name, const QString &modelfilePath)
{
    auto *process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus) {
        bool success = (exitCode == 0);
        QString output = QString::fromUtf8(process->readAllStandardOutput())
                       + QString::fromUtf8(process->readAllStandardError());
        emit createFinished(success, output.trimmed());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
        emit createFinished(false, process->errorString());
        process->deleteLater();
    });
    process->start(m_ollamaPath, {"create", name, "-f", modelfilePath});
}

void OllamaCli::deleteModelAsync(const QString &name)
{
    auto *process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus) {
        bool success = (exitCode == 0);
        QString output = QString::fromUtf8(process->readAllStandardOutput())
                       + QString::fromUtf8(process->readAllStandardError());
        emit deleteFinished(success, output.trimmed());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
        emit deleteFinished(false, process->errorString());
        process->deleteLater();
    });
    process->start(m_ollamaPath, {"rm", name});
}

void OllamaCli::copyModelAsync(const QString &oldName, const QString &newName)
{
    auto *process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus) {
        bool success = (exitCode == 0);
        QString output = QString::fromUtf8(process->readAllStandardOutput())
                       + QString::fromUtf8(process->readAllStandardError());
        emit copyFinished(success, output.trimmed());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError) {
        emit copyFinished(false, process->errorString());
        process->deleteLater();
    });
    process->start(m_ollamaPath, {"cp", oldName, newName});
}

QList<OllamaCli::ModelInfo> OllamaCli::parseListOutput(const QString &output)
{
    QList<ModelInfo> models;
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    for (int i = 1; i < lines.size(); ++i) {
        const QString &line = lines[i];
        if (line.trimmed().isEmpty()) continue;

        static const QRegularExpression re("\\s{2,}");
        QStringList parts = line.split(re, Qt::SkipEmptyParts);
        if (parts.size() >= 4) {
            ModelInfo info;
            info.name = parts[0];
            info.id = parts[1];
            info.size = parts[2];
            info.modified = parts.mid(3).join(" ");
            models.append(info);
        }
    }
    return models;
}

OllamaCli::ModelParams OllamaCli::parseShowOutput(const QString &output)
{
    ModelParams params;
    const QStringList lines = output.split('\n');

    enum Section { None, Model, Capabilities, Parameters, System, License };
    Section currentSection = None;

    static const QRegularExpression kvRe("\\s{2,}");

    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) continue;

        if (line.startsWith("  ") && !line.startsWith("    ")) {
            QString section = line.trimmed();
            if (section == "Model") currentSection = Model;
            else if (section == "Capabilities") currentSection = Capabilities;
            else if (section == "Parameters") currentSection = Parameters;
            else if (section == "System") currentSection = System;
            else if (section == "License") currentSection = License;
            else currentSection = None;
            continue;
        }

        QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;

        switch (currentSection) {
        case Model: {
            QStringList kv = trimmed.split(kvRe, Qt::SkipEmptyParts);
            if (kv.size() >= 2) {
                QString key = kv[0].trimmed();
                QString value = kv[1].trimmed();
                if (key == "architecture") params.architecture = value;
                else if (key == "parameters") params.paramCount = value;
                else if (key == "context length") params.contextLength = value.toInt();
                else if (key == "quantization") params.quantization = value;
                else if (key == "renderer") params.renderer = value;
                else if (key == "parser") params.parser = value;
            }
            break;
        }
        case Capabilities: {
            // Nested details (e.g. "levels false, low, medium") must not be
            // mistaken for capabilities.
            static const QStringList known = {
                QStringLiteral("completion"), QStringLiteral("tools"),
                QStringLiteral("thinking"), QStringLiteral("vision"),
                QStringLiteral("audio"), QStringLiteral("embedding")
            };
            if (known.contains(trimmed))
                params.capabilities.append(trimmed);
            break;
        }
        case Parameters: {
            QStringList kv = trimmed.split(kvRe, Qt::SkipEmptyParts);
            if (kv.size() >= 2) {
                params.parameters[kv[0].trimmed()] = kv[1].trimmed();
            }
            break;
        }
        case System:
            if (!params.systemPrompt.isEmpty()) params.systemPrompt += "\n";
            params.systemPrompt += trimmed;
            break;
        case License:
            break;
        default:
            break;
        }
    }

    return params;
}

QString OllamaCli::extractFromBase(const QString &modelfileContent)
{    static const QRegularExpression commentFromRe(
        "^#\\s*FROM\\s+(.+)$", QRegularExpression::MultilineOption);
    QRegularExpressionMatch cmtMatch = commentFromRe.match(modelfileContent);
    if (cmtMatch.hasMatch()) {
        return cmtMatch.captured(1).trimmed();
    }

    static const QRegularExpression fromRe("^FROM\\s+(.+)$",
                                            QRegularExpression::MultilineOption);
    QRegularExpressionMatch match = fromRe.match(modelfileContent);
    if (match.hasMatch()) {
        QString from = match.captured(1).trimmed();
        if (!from.startsWith('/')) {
            return from;
        }
    }
    return QString();
}

QString OllamaCli::getModelFromBlob(const QString &name)
{
    QString modelfile = showModelfile(name);
    if (modelfile.isEmpty()) return QString();

    static const QRegularExpression fromLineRe(
        "^FROM\\s+(/var/lib/ollama/blobs/sha256-[a-f0-9]+)",
        QRegularExpression::MultilineOption);
    QRegularExpressionMatch match = fromLineRe.match(modelfile);
    if (match.hasMatch()) {
        return match.captured(1).trimmed();
    }
    return QString();
}

int OllamaCli::countBlobReferences(const QString &blob)
{
    if (blob.isEmpty()) return 0;

    QList<ModelInfo> models = listModels();
    int count = 0;

    for (const auto &model : models) {
        QString otherBlob = getModelFromBlob(model.name);
        if (otherBlob == blob) {
            ++count;
        }
    }
    return count;
}

QString OllamaCli::findOtherModelSharingBlob(const QString &blob, const QString &excludeName)
{
    if (blob.isEmpty()) return QString();

    QList<ModelInfo> models = listModels();
    QString bestMatch;

    for (const auto &model : models) {
        if (model.name == excludeName) continue;
        QString otherBlob = getModelFromBlob(model.name);
        if (otherBlob == blob) {
            if (bestMatch.isEmpty() || model.name.length() < bestMatch.length()) {
                bestMatch = model.name;
            }
        }
    }
    return bestMatch;
}

// Extracts the value of a Modelfile directive (TEMPLATE, RENDERER, PARSER)
// from `ollama show --modelfile` output. Triple-quoted values may span lines
// and the closing quotes may sit on their own line or right after the last
// character; single-quoted values are unquoted.
QString OllamaCli::extractDirective(const QString &modelfileContent, const QString &directive)
{
    static const QString tripleQuote = QStringLiteral("\"\"\"");

    const QRegularExpression re(
        QStringLiteral("^%1[ \\t]+(.*)$").arg(QRegularExpression::escape(directive)),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = re.match(modelfileContent);
    if (!match.hasMatch())
        return QString();

    QString value = match.captured(1).trimmed();

    if (value.startsWith(tripleQuote)) {
        const int openPos = modelfileContent.indexOf(tripleQuote, match.capturedStart(1));
        const int closePos = openPos < 0 ? -1
                                         : modelfileContent.indexOf(tripleQuote, openPos + 3);
        if (closePos > openPos)
            return modelfileContent.mid(openPos + 3, closePos - openPos - 3).trimmed();
    }

    if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')) && value.size() >= 2) {
        value = value.mid(1, value.size() - 2);
        value.replace(QLatin1String("\\\""), QLatin1String("\""));
    }
    return value;
}

bool OllamaCli::isPulling() const
{
    return m_pull != nullptr;
}

void OllamaCli::listModelsAsync()
{
    if (m_list) {
        m_list->kill();
        m_list->deleteLater();
        m_list = nullptr;
    }

    auto *process = new QProcess(this);
    m_list = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process](int exitCode, QProcess::ExitStatus status) {
        const QString output = QString::fromUtf8(process->readAllStandardOutput());
        const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (m_list == process)
            m_list = nullptr;
        process->deleteLater();
        const bool ok = exitCode == 0 && status == QProcess::NormalExit;
        emit modelsListed(ok, ok ? parseListOutput(output) : QList<ModelInfo>(), error);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        const QString message = process->errorString();
        if (m_list == process)
            m_list = nullptr;
        process->deleteLater();
        emit modelsListed(false, {}, message);
    });
    process->start(m_ollamaPath, {QStringLiteral("list")});
}

void OllamaCli::pullModelAsync(const QString &name)
{
    if (m_pull) {
        m_pull->kill();
        m_pull->deleteLater();
        m_pull = nullptr;
    }

    m_pullBuffer.clear();
    m_lastPullStatus.clear();
    m_lastPullPercent = -2;
    auto *process = new QProcess(this);
    m_pull = process;
    process->setProcessChannelMode(QProcess::MergedChannels);

    auto handleChunk = [this]() {
        if (!m_pull)
            return;
        m_pullBuffer += QString::fromUtf8(m_pull->readAll());
        int newline;
        while ((newline = m_pullBuffer.indexOf(QLatin1Char('\n'))) >= 0) {
            QString line = m_pullBuffer.left(newline);
            m_pullBuffer.remove(0, newline + 1);
            // Strip ANSI escapes and carriage-return redraws.
            line.remove(QRegularExpression(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]")));
            line = line.trimmed();
            if (line.isEmpty())
                continue;

            static const QRegularExpression percentRe(QStringLiteral("(\\d+)%"));
            const QRegularExpressionMatch percentMatch = percentRe.match(line);
            const int percent = percentMatch.hasMatch() ? percentMatch.captured(1).toInt() : -1;

            QString status = line;
            static const QRegularExpression sizeRe(
                QStringLiteral("(\\d+(?:\\.\\d+)?\\s*[KMG]B)\\s*/\\s*(\\d+(?:\\.\\d+)?\\s*[KMG]B)"));
            const QRegularExpressionMatch sizeMatch = sizeRe.match(line);
            if (sizeMatch.hasMatch())
                status = sizeMatch.captured(1) + QStringLiteral(" / ") + sizeMatch.captured(2);
            else if (line.startsWith(QLatin1String("pulling ")))
                status = line.section(QLatin1Char(':'), 0, 0);

            // Ollama redraws the same line many times; emit only changes.
            if (status == m_lastPullStatus && percent == m_lastPullPercent)
                continue;
            m_lastPullStatus = status;
            m_lastPullPercent = percent;
            emit pullProgress(status, percent);
        }
    };

    connect(process, &QProcess::readyRead, this, handleChunk);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, process, handleChunk](int exitCode, QProcess::ExitStatus status) {
        handleChunk();
        const QString output = QString::fromUtf8(process->readAll()).trimmed();
        if (m_pull == process)
            m_pull = nullptr;
        process->deleteLater();
        emit pullFinished(exitCode == 0 && status == QProcess::NormalExit, output);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            if (m_pull == process)
                m_pull = nullptr;
            const QString err = process->errorString();
            process->deleteLater();
            emit pullFinished(false, err);
        }
    });

    process->start(m_ollamaPath, {QStringLiteral("pull"), name});
}

void OllamaCli::cancelPull()
{
    if (m_pull)
        m_pull->kill();
}

void OllamaCli::renameModelAsync(const QString &oldName, const QString &newName)
{
    auto *copy = new QProcess(this);
    connect(copy, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this, copy, oldName](int exitCode, QProcess::ExitStatus status) {
        const QString copyOutput = QString::fromUtf8(copy->readAllStandardOutput())
                + QString::fromUtf8(copy->readAllStandardError());
        copy->deleteLater();
        if (exitCode != 0 || status != QProcess::NormalExit) {
            emit renameFinished(false, copyOutput.trimmed());
            return;
        }

        auto *remove = new QProcess(this);
        connect(remove, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                [this, remove](int rmExit, QProcess::ExitStatus rmStatus) {
            const QString rmOutput = QString::fromUtf8(remove->readAllStandardOutput())
                    + QString::fromUtf8(remove->readAllStandardError());
            remove->deleteLater();
            emit renameFinished(rmExit == 0 && rmStatus == QProcess::NormalExit,
                                rmOutput.trimmed());
        });
        remove->start(m_ollamaPath, {QStringLiteral("rm"), oldName});
    });
    connect(copy, &QProcess::errorOccurred, this, [this, copy](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        const QString message = copy->errorString();
        copy->deleteLater();
        emit renameFinished(false, message);
    });
    copy->start(m_ollamaPath, {QStringLiteral("cp"), oldName, newName});
}
