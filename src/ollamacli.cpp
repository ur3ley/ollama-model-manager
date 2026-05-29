#include "ollamacli.h"
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
            }
            break;
        }
        case Capabilities:
            params.capabilities.append(trimmed);
            break;
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
{
    static const QRegularExpression commentFromRe(
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
