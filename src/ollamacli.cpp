#include "ollamacli.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>

#include <utility>

// ---------------------------------------------------------------- OllamaJob

OllamaJob::OllamaJob(OllamaCli *cli, quint64 id, Kind kind, const QString &name)
    : QObject(cli)
    , m_cli(cli)
    , m_id(id)
    , m_kind(kind)
    , m_name(name)
{
}

void OllamaJob::cancel()
{
    if (m_kind == Kind::Pull && m_state != State::Finished)
        m_cli->cancelJob(this);
}

void OllamaJob::setFinishHook(std::function<void(OllamaJob *)> hook)
{
    m_finishHook = std::move(hook);
}

// ---------------------------------------------------------------- OllamaCli

OllamaCli::OllamaCli(QObject *parent, const QString &ollamaPath)
    : QObject(parent)
    , m_ollamaPath(ollamaPath.isEmpty() ? QStandardPaths::findExecutable(QStringLiteral("ollama"))
                                        : ollamaPath)
{
}

bool OllamaCli::isAvailable() const
{
    return !m_ollamaPath.isEmpty();
}

QString OllamaCli::ollamaPath() const
{
    return m_ollamaPath;
}

bool OllamaCli::isValidModelName(const QString &name)
{
    static const QRegularExpression nameRe(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:/-]*$"));
    return name.size() <= 512 && nameRe.match(name).hasMatch();
}

OllamaJob *OllamaCli::newJob(OllamaJob::Kind kind, const QString &name)
{
    return new OllamaJob(this, m_nextJobId++, kind, name);
}

void OllamaCli::schedule(OllamaJob *job, const QStringList &names, std::function<void()> start)
{
    // Queued even on success: the caller connects to the job after we return.
    QMetaObject::invokeMethod(job, [this, job, names, start = std::move(start)]() {
        if (m_ollamaPath.isEmpty()) {
            finish(job, false, QStringLiteral("ollama not found in PATH"));
            return;
        }
        for (const QString &name : names) {
            if (!isValidModelName(name)) {
                finish(job, false, QStringLiteral("invalid model name: \"%1\"").arg(name));
                return;
            }
        }
        start();
    }, Qt::QueuedConnection);
}

void OllamaCli::runStep(OllamaJob *job, const QStringList &args, const StepDone &done)
{
    job->m_state = OllamaJob::State::Running;
    auto *process = new QProcess(job);
    job->m_process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    // A crash also reports finished(CrashExit): only a failed start has no
    // finished() and is answered from errorOccurred — exactly one result (B-4).
    connect(process, &QProcess::finished, job,
            [job, process, done](int exitCode, QProcess::ExitStatus status) {
        const QString out = QString::fromUtf8(process->readAllStandardOutput());
        const QString err = QString::fromUtf8(process->readAllStandardError());
        if (job->m_process == process)
            job->m_process = nullptr;
        process->deleteLater();
        done(exitCode == 0 && status == QProcess::NormalExit, out, err);
    });
    connect(process, &QProcess::errorOccurred, job, [job, process, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        const QString message = process->errorString();
        if (job->m_process == process)
            job->m_process = nullptr;
        process->deleteLater();
        done(false, QString(), message);
    });
    process->start(m_ollamaPath, args);
}

void OllamaCli::finish(OllamaJob *job, bool success, const QString &output)
{
    if (job->m_state == OllamaJob::State::Finished)
        return;
    job->m_state = OllamaJob::State::Finished;
    job->m_success = success;
    job->m_output = output;
    // The owner's bookkeeping (provenance, freed bytes) runs before the signals,
    // so a consumer's slot sees the final result whatever the connect order was.
    if (auto hook = std::move(job->m_finishHook)) {
        job->m_finishHook = nullptr;
        hook(job);
    }
    if (m_activePull == job)
        m_activePull = nullptr;
    // Whatever path led here, a pull that leaves the pipeline must hand its
    // queue slot back — a rejected job would otherwise keep isPulling() true.
    const bool wasQueued = job->m_kind == OllamaJob::Kind::Pull && m_pullQueue.removeAll(job) > 0;
    // deleteLater() comes before the signals: a receiver of jobFinished() may
    // destroy the whole cli, and then the job is gone with it.
    job->deleteLater();
    emit job->finished(success, output);
    if (wasQueued)
        startNextPull();
    notifyPullActivity();
    emit jobFinished(job);
}

void OllamaCli::listModelsAsync()
{
    if (m_list) {
        // The superseded process must stay silent: its finished(CrashExit)
        // would report a failed list while the new request is still running.
        disconnect(m_list, nullptr, this, nullptr);
        m_list->kill();
        m_list->deleteLater();
        m_list = nullptr;
    }
    if (m_ollamaPath.isEmpty()) {
        QMetaObject::invokeMethod(this, [this]() {
            emit modelsListed(false, {}, QStringLiteral("ollama not found in PATH"));
        }, Qt::QueuedConnection);
        return;
    }

    auto *process = new QProcess(this);
    m_list = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::finished, this,
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

OllamaJob *OllamaCli::showModelAsync(const QString &name)
{
    OllamaJob *job = newJob(OllamaJob::Kind::Show, name);
    schedule(job, {name}, [this, job, name]() {
        runStep(job, {QStringLiteral("show"), name},
                [this, job, name](bool ok, const QString &out, const QString &err) {
            if (!ok) {
                finish(job, false, err.trimmed());
                return;
            }
            job->m_params = parseShowOutput(out);
            // A failed --modelfile still leaves the `show` details usable.
            runStep(job, {QStringLiteral("show"), name, QStringLiteral("--modelfile")},
                    [this, job](bool mfOk, const QString &content, const QString &) {
                if (mfOk) {
                    applyModelfile(job->m_params, content);
                    learnModelsDir(content);
                }
                finish(job, true, QString());
            });
        });
    });
    return job;
}

OllamaJob *OllamaCli::createModel(const QString &name, const QString &modelfileContent)
{
    OllamaJob *job = newJob(OllamaJob::Kind::Create, name);
    // QTemporaryFile creates the file exclusively (O_EXCL) with mode 0600 under
    // a random name: no shared predictable path, no symlink or swap race, and
    // the system prompt is not readable by other local users (S-1). It lives
    // exactly as long as this job.
    auto *file = new QTemporaryFile(
            QDir::temp().filePath(QStringLiteral("ollama-modelfile-XXXXXX.tmp")), job);
    const QByteArray data = modelfileContent.toUtf8();
    QString writeError;
    if (!file->open() || file->write(data) != data.size() || !file->flush())
        writeError = QStringLiteral("cannot write temporary Modelfile %1: %2")
                .arg(file->fileName(), file->errorString());
    file->close();  // the file stays on disk until the job is deleted
    job->m_modelfile = file;

    schedule(job, {name}, [this, job, name, writeError]() {
        if (!writeError.isEmpty()) {
            finish(job, false, writeError);
            return;
        }
        runStep(job, {QStringLiteral("create"), name, QStringLiteral("-f"), job->m_modelfile->fileName()},
                [this, job](bool ok, const QString &out, const QString &err) {
            finish(job, ok, (out + err).trimmed());
        });
    });
    return job;
}

OllamaJob *OllamaCli::deleteModelAsync(const QString &name)
{
    OllamaJob *job = newJob(OllamaJob::Kind::Delete, name);
    schedule(job, {name}, [this, job, name]() {
        runStep(job, {QStringLiteral("rm"), name},
                [this, job](bool ok, const QString &out, const QString &err) {
            finish(job, ok, (out + err).trimmed());
        });
    });
    return job;
}

OllamaJob *OllamaCli::copyModelAsync(const QString &oldName, const QString &newName)
{
    OllamaJob *job = newJob(OllamaJob::Kind::Copy, newName);
    job->m_source = oldName;
    schedule(job, {oldName, newName}, [this, job, oldName, newName]() {
        runStep(job, {QStringLiteral("cp"), oldName, newName},
                [this, job](bool ok, const QString &out, const QString &err) {
            finish(job, ok, (out + err).trimmed());
        });
    });
    return job;
}

OllamaJob *OllamaCli::renameModelAsync(const QString &oldName, const QString &newName)
{
    OllamaJob *job = newJob(OllamaJob::Kind::Rename, newName);
    job->m_source = oldName;
    schedule(job, {oldName, newName}, [this, job, oldName]() {
        runStep(job, {QStringLiteral("cp"), job->m_source, job->m_name},
                [this, job, oldName](bool ok, const QString &out, const QString &err) {
            if (!ok) {
                finish(job, false, (out + err).trimmed());
                return;
            }
            runStep(job, {QStringLiteral("rm"), oldName},
                    [this, job](bool rmOk, const QString &rmOut, const QString &rmErr) {
                // The copy already exists: a failed `rm` is a partial success,
                // not a plain failure — the caller must not lose the new name (B-5).
                job->m_partial = !rmOk;
                finish(job, rmOk, (rmOut + rmErr).trimmed());
            });
        });
    });
    return job;
}

bool OllamaCli::isPulling() const
{
    return m_activePull || !m_pullQueue.isEmpty();
}

QStringList OllamaCli::stalePartialBlobs() const
{
    QStringList files;
    const QString root = modelsDir();
    if (root.isEmpty())
        return files;
    const QDir blobs(QDir(root).filePath(QStringLiteral("blobs")));
    const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-3600);
    const QFileInfoList entries = blobs.entryInfoList(QDir::Files);
    for (const QFileInfo &entry : entries) {
        // Only `sha256-<hex>-partial[-N]`; complete blobs and manifests never
        // contain this marker, so installed models cannot be hit.
        if (!entry.fileName().contains(QLatin1String("-partial")))
            continue;
        if (entry.lastModified() > cutoff)
            continue;
        files.append(entry.absoluteFilePath());
    }
    return files;
}

QList<OllamaCli::PullInfo> OllamaCli::pullList() const
{
    QList<PullInfo> out;
    if (m_activePull)
        out.append(PullInfo{m_activePull->id(), m_activePull->name(), true,
                            m_activePull->m_lastPercent});
    for (const QPointer<OllamaJob> &job : m_pullQueue) {
        if (job)
            out.append(PullInfo{job->id(), job->name(), false, -1});
    }
    return out;
}

bool OllamaCli::cancelPull(quint64 id)
{
    if (m_activePull && m_activePull->id() == id) {
        cancelJob(m_activePull);
        return true;
    }
    for (const QPointer<OllamaJob> &job : std::as_const(m_pullQueue)) {
        if (job && job->id() == id) {
            cancelJob(job);
            return true;
        }
    }
    return false;
}

bool OllamaCli::movePull(quint64 id, int delta)
{
    if (delta == 0)
        return false;
    int index = -1;
    for (int i = 0; i < m_pullQueue.size(); ++i) {
        if (m_pullQueue.at(i) && m_pullQueue.at(i)->id() == id) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return false;
    const int target = index + delta;
    if (target < 0 || target >= m_pullQueue.size())
        return false;
    m_pullQueue.move(index, target);
    notifyPullActivity();
    return true;
}

bool OllamaCli::movePullBefore(quint64 draggedId, quint64 targetId)
{
    int from = -1;
    int to = -1;
    for (int i = 0; i < m_pullQueue.size(); ++i) {
        if (!m_pullQueue.at(i))
            continue;
        if (m_pullQueue.at(i)->id() == draggedId)
            from = i;
        if (m_pullQueue.at(i)->id() == targetId)
            to = i;
    }
    if (from < 0 || to < 0 || from == to)
        return false;
    if (from < to)
        --to;
    m_pullQueue.move(from, to);
    notifyPullActivity();
    return true;
}

bool OllamaCli::prioritizePull(quint64 id)
{
    int index = -1;
    for (int i = 0; i < m_pullQueue.size(); ++i) {
        if (m_pullQueue.at(i) && m_pullQueue.at(i)->id() == id) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return false;
    m_pullQueue.move(index, 0);
    if (!m_activePull) {
        startNextPull();
        notifyPullActivity();
        return true;
    }
    OllamaJob *active = m_activePull;
    if (active->m_process) {
        // Stop the running `ollama pull`; its finished handler puts the job
        // right behind the promoted one and starts the queue again.
        active->m_requeue = true;
        active->m_process->kill();
        return true;
    }
    // Scheduled but not started yet: requeue it by hand.
    if (m_activePull == active)
        m_activePull = nullptr;
    active->m_state = OllamaJob::State::Queued;
    m_pullQueue.insert(1, active);
    emit pullSuspended(active->id());
    notifyPullActivity();
    startNextPull();
    return true;
}

void OllamaCli::notifyPullActivity()
{
    emit pullActivityChanged(m_activePull ? 1 : 0, m_pullQueue.size());
}

OllamaJob *OllamaCli::pullModelAsync(const QString &name, qint64 expectedBytes)
{
    OllamaJob *job = newJob(OllamaJob::Kind::Pull, name);
    job->m_expectedBytes = expectedBytes;
    // Join the queue only if the pull can actually run: schedule() refuses an
    // invalid name or a missing binary without calling startNextPull(), and a
    // stale entry would leave isPulling() true for good (review 2026-11-22, 3.1).
    if (isValidModelName(name) && !m_ollamaPath.isEmpty())
        m_pullQueue.append(job);
    schedule(job, {name}, [this]() { startNextPull(); });
    if (m_pullQueue.contains(job))
        notifyPullActivity();
    return job;
}

void OllamaCli::startNextPull()
{
    if (m_activePull)
        return;
    while (!m_pullQueue.isEmpty()) {
        OllamaJob *next = m_pullQueue.takeFirst();
        if (!next || next->isFinished())
            continue;
        startPull(next);
        return;
    }
}

void OllamaCli::startPull(OllamaJob *job)
{
    m_activePull = job;
    job->m_state = OllamaJob::State::Running;
    emit pullStarted(job->id());
    notifyPullActivity();
    job->m_pullParser.reset();
    job->m_pullParser.setExpectedBytes(job->m_expectedBytes);

    auto *process = new QProcess(job);
    job->m_process = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyRead, job, [this, job]() { handlePullOutput(job); });
    connect(process, &QProcess::finished, job,
            [this, job, process](int exitCode, QProcess::ExitStatus status) {
        handlePullOutput(job);
        // The last line often has no trailing newline ("success").
        if (!job->m_pullBuffer.trimmed().isEmpty()) {
            job->m_lastLine = job->m_pullBuffer.trimmed();
            const PullProgressParser::Update update = job->m_pullParser.feed(job->m_pullBuffer);
            job->m_pullBuffer.clear();
            emit job->progress(update.label, update.percent, update.received, update.total);
        }
        job->m_process = nullptr;
        process->deleteLater();
        if (job->m_requeue) {
            // Deliberately stopped by prioritizePull(): back to the queue right
            // behind the promoted pull, no finished() and no failure state.
            job->m_requeue = false;
            job->m_cancelled = false;
            job->m_state = OllamaJob::State::Queued;
            job->m_pullParser.reset();
            job->m_pullBuffer.clear();
            job->m_lastPercent = -2;
            job->m_lastLabel.clear();
            job->m_lastReceived = -1;
            if (m_activePull == job)
                m_activePull = nullptr;
            m_pullQueue.insert(qMin(1, m_pullQueue.size()), job);
            emit pullSuspended(job->id());
            notifyPullActivity();
            startNextPull();
            return;
        }
        const bool ok = exitCode == 0 && status == QProcess::NormalExit && !job->m_cancelled;
        if (ok) {
            // Ollama redraws over the last frames, so the biggest layer may end
            // at 99%: the exit code is what makes the bar full.
            const PullProgressParser::Update update = job->m_pullParser.markComplete();
            emit job->progress(update.label, update.percent, update.received, update.total);
        }
        if (m_activePull == job)
            m_activePull = nullptr;
        finish(job, ok, job->m_cancelled ? QStringLiteral("cancelled") : job->m_lastLine);
        startNextPull();
    });
    connect(process, &QProcess::errorOccurred, job, [this, job, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        const QString message = process->errorString();
        job->m_process = nullptr;
        process->deleteLater();
        if (m_activePull == job)
            m_activePull = nullptr;
        finish(job, false, message);
        startNextPull();
    });
    process->start(m_ollamaPath, {QStringLiteral("pull"), job->m_name});
}

void OllamaCli::handlePullOutput(OllamaJob *job)
{
    if (!job->m_process)
        return;
    job->m_pullBuffer += QString::fromUtf8(job->m_process->readAll());
    int newline;
    while ((newline = job->m_pullBuffer.indexOf(QLatin1Char('\n'))) >= 0) {
        const QString line = job->m_pullBuffer.left(newline);
        job->m_pullBuffer.remove(0, newline + 1);
        if (!line.trimmed().isEmpty())
            job->m_lastLine = line.trimmed();
        // ANSI escapes and '\r' redraws are handled by the parser.
        const PullProgressParser::Update update = job->m_pullParser.feed(line);
        if (update.percent == job->m_lastPercent && update.label == job->m_lastLabel
                && update.received == job->m_lastReceived)
            continue;
        job->m_lastPercent = update.percent;
        job->m_lastLabel = update.label;
        job->m_lastReceived = update.received;
        emit job->progress(update.label, update.percent, update.received, update.total);
    }
}

void OllamaCli::cancelJob(OllamaJob *job)
{
    job->m_cancelled = true;
    if (job->m_process) {
        // finished(CrashExit) follows and reports the cancellation.
        job->m_process->kill();
        return;
    }
    // Still queued: never started, nothing to kill.
    m_pullQueue.removeAll(job);
    finish(job, false, QStringLiteral("cancelled"));
}

OllamaJob *OllamaCli::blobUsageAsync(const QString &name)
{
    OllamaJob *job = newJob(OllamaJob::Kind::BlobUsage, name);
    schedule(job, {name}, [this, job, name]() {
        runStep(job, {QStringLiteral("show"), name, QStringLiteral("--modelfile")},
                [this, job](bool ok, const QString &content, const QString &err) {
            if (!ok) {
                finish(job, false, err.trimmed());
                return;
            }
            learnModelsDir(content);
            job->m_blob = blobFromModelfile(content);
            if (job->m_blob.isEmpty()) {
                // FROM a model name, not a blob: nothing is shared on disk.
                finish(job, true, QString());
                return;
            }
            // Own `ollama list`: the shared one may be replaced by a refresh.
            runStep(job, {QStringLiteral("list")},
                    [this, job](bool listOk, const QString &out, const QString &listErr) {
                if (!listOk) {
                    finish(job, false, listErr.trimmed());
                    return;
                }
                job->m_pendingModels = parseListOutput(out);
                inspectNextBlob(job);
            });
        });
    });
    return job;
}

void OllamaCli::inspectNextBlob(OllamaJob *job)
{
    // Cached ids cost nothing; the first unknown one costs one process.
    while (!job->m_pendingModels.isEmpty()) {
        const ModelInfo model = job->m_pendingModels.first();
        const auto cached = m_blobById.constFind(model.id);
        if (model.id.isEmpty() || cached == m_blobById.constEnd())
            break;
        job->m_pendingModels.removeFirst();
        if (*cached == job->m_blob)
            ++job->m_blobRefs;
    }
    if (job->m_pendingModels.isEmpty()) {
        finish(job, true, QString());
        return;
    }
    const ModelInfo model = job->m_pendingModels.takeFirst();
    if (!isValidModelName(model.name)) {
        inspectNextBlob(job);
        return;
    }
    runStep(job, {QStringLiteral("show"), model.name, QStringLiteral("--modelfile")},
            [this, job, model](bool ok, const QString &content, const QString &) {
        if (ok) {
            const QString blob = blobFromModelfile(content);
            if (!model.id.isEmpty())
                m_blobById.insert(model.id, blob);
            if (blob == job->m_blob)
                ++job->m_blobRefs;
        }
        inspectNextBlob(job);
    });
}

QString OllamaCli::blobFromModelfile(const QString &modelfileContent)
{
    // Any storage root: /var/lib/ollama, ~/.ollama/models, $OLLAMA_MODELS (B-8).
    static const QRegularExpression fromBlobRe(
        QStringLiteral("^FROM\\s+(/.*/blobs/sha256-[a-f0-9]+)\\s*$"),
        QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = fromBlobRe.match(modelfileContent);
    return match.hasMatch() ? match.captured(1) : QString();
}

QString OllamaCli::modelsDirFromBlob(const QString &blobPath)
{
    const int at = blobPath.lastIndexOf(QStringLiteral("/blobs/"));
    return at > 0 ? blobPath.left(at) : QString();
}

QString OllamaCli::modelsDir() const
{
    // Same lookup order as the daemon: explicit OLLAMA_MODELS first. The GUI
    // usually does not see the service environment, so a root learned from a
    // real `FROM <blob>` beats the guesses below.
    const QString env = qEnvironmentVariable("OLLAMA_MODELS").trimmed();
    if (!env.isEmpty())
        return env;
    if (!m_modelsDir.isEmpty())
        return m_modelsDir;
    const QStringList candidates = {
        QStringLiteral("/var/lib/ollama"),                  // distro packages
        QStringLiteral("/usr/share/ollama/.ollama/models"), // official install.sh
        QDir::home().filePath(QStringLiteral(".ollama/models")), // `ollama serve` as the user
    };
    for (const QString &dir : candidates) {
        if (QFileInfo(QDir(dir).filePath(QStringLiteral("blobs"))).isDir())
            return dir;
    }
    return QString();
}

void OllamaCli::learnModelsDir(const QString &modelfileContent)
{
    const QString dir = modelsDirFromBlob(blobFromModelfile(modelfileContent));
    if (!dir.isEmpty())
        m_modelsDir = dir;
}

void OllamaCli::applyModelfile(ModelParams &params, const QString &modelfileContent)
{
    if (modelfileContent.isEmpty())
        return;
    params.fromBase = extractFromBase(modelfileContent);
    // The Modelfile keeps the prompt verbatim (blank lines, indentation); the
    // "System" section of plain `ollama show` is re-flowed and lossy.
    params.systemPrompt = extractDirective(modelfileContent, QStringLiteral("SYSTEM"));
    params.templateStr = extractDirective(modelfileContent, QStringLiteral("TEMPLATE"));
    params.renderer = extractDirective(modelfileContent, QStringLiteral("RENDERER"));
    params.parser = extractDirective(modelfileContent, QStringLiteral("PARSER"));
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
                params.parameters.insert(kv[0].trimmed(), kv[1].trimmed());
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
        // `FROM "/path with spaces/x.gguf"` (ModelfileBuilder::fromLine) is a path too.
        if (from.size() >= 2 && from.startsWith(QLatin1Char('"')) && from.endsWith(QLatin1Char('"')))
            from = from.mid(1, from.size() - 2);
        if (!from.startsWith('/')) {
            return from;
        }
    }
    return QString();
}

// Extracts the value of a Modelfile directive (SYSTEM, TEMPLATE, RENDERER, PARSER)
// from `ollama show --modelfile` output. Triple-quoted values may span lines
// and the closing quotes may sit on their own line or right after the last
// character; single-quoted values are unquoted.
QString OllamaCli::extractDirective(const QString &modelfileContent, const QString &directive)
{
    static const QString tripleQuote = QStringLiteral("\"\"\"");

    // Compiled once per directive (P-5): the editor asks for 4 directives per model.
    static QHash<QString, QRegularExpression> cache;
    auto it = cache.find(directive);
    if (it == cache.end()) {
        it = cache.insert(directive, QRegularExpression(
                QStringLiteral("^%1[ \\t]+(.*)$").arg(QRegularExpression::escape(directive)),
                QRegularExpression::MultilineOption));
    }
    const QRegularExpression &re = *it;
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

