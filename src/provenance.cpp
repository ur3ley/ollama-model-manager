#include "provenance.h"

#include "apppaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

Provenance &Provenance::instance()
{
    static Provenance store;
    return store;
}

QString Provenance::storePath()
{
    return QDir(apppaths::configDir()).filePath(QStringLiteral("models.json"));
}

QString Provenance::legacyStorePath()
{
    // Up to 2026-11: AppConfigLocation, i.e. ~/.config/OllamaManager/
    // Ollama Model Manager/models.json (depends on the app/org names).
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir.isEmpty() ? QDir::homePath() : dir).filePath(QStringLiteral("models.json"));
}

bool Provenance::migrateLegacy(const QString &from, const QString &to)
{
    if (from.isEmpty() || QFileInfo(from).absoluteFilePath() == QFileInfo(to).absoluteFilePath())
        return false;
    // Never overwrite: once the new file exists it is the only source of truth.
    if (QFileInfo::exists(to) || !QFileInfo::exists(from))
        return false;
    const QString dir = QFileInfo(to).absolutePath();
    if (dir == apppaths::configDir())
        apppaths::ensurePrivateDir(dir);
    else
        QDir().mkpath(dir);
    if (!QFile::rename(from, to)) {
        // Different filesystem (bind-mounted config, …): copy, keep the source.
        if (!QFile::copy(from, to))
            return false;
    }
    apppaths::restrictFile(to);
    return true;
}

Provenance::Provenance()
    : m_path(storePath())
{
    migrateLegacy(legacyStorePath(), m_path);
    load();
}

Provenance::Provenance(const QString &path, QObject *parent)
    : QObject(parent)
    , m_path(path)
{
    load();
}

void Provenance::load()
{
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isObject())
        m_data = doc.object();
}

bool Provenance::save() const
{
    const QString &path = m_path;
    apppaths::ensurePrivateDir(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(m_data).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return false;
    apppaths::restrictFile(path);  // download history: owner only (S-8)
    return true;
}

void Provenance::record(const QString &modelName, const Entry &entry)
{
    if (modelName.isEmpty() || entry.isEmpty())
        return;

    QJsonObject obj;
    obj.insert(QStringLiteral("source"), entry.source);
    obj.insert(QStringLiteral("ref"), entry.ref);
    if (!entry.file.isEmpty())
        obj.insert(QStringLiteral("file"), entry.file);
    if (!entry.digest.isEmpty())
        obj.insert(QStringLiteral("digest"), entry.digest);
    if (!entry.sha.isEmpty())
        obj.insert(QStringLiteral("sha"), entry.sha);
    obj.insert(QStringLiteral("installedAt"),
               entry.installedAt.isEmpty()
                       ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
                       : entry.installedAt);

    m_data.insert(modelName, obj);
    save();
    emit changed(modelName);
}

Provenance::Entry Provenance::lookup(const QString &modelName) const
{
    Entry entry;
    const QJsonObject obj = m_data.value(modelName).toObject();
    entry.source = obj.value(QStringLiteral("source")).toString();
    entry.ref = obj.value(QStringLiteral("ref")).toString();
    entry.file = obj.value(QStringLiteral("file")).toString();
    entry.digest = obj.value(QStringLiteral("digest")).toString();
    entry.sha = obj.value(QStringLiteral("sha")).toString();
    entry.installedAt = obj.value(QStringLiteral("installedAt")).toString();
    return entry;
}

bool Provenance::contains(const QString &modelName) const
{
    return m_data.contains(modelName);
}

void Provenance::remove(const QString &modelName)
{
    if (!m_data.contains(modelName))
        return;
    m_data.remove(modelName);
    save();
    emit changed(modelName);
}

void Provenance::rename(const QString &oldName, const QString &newName)
{
    if (oldName.isEmpty() || newName.isEmpty() || oldName == newName)
        return;
    if (!m_data.contains(oldName))
        return;

    // take() keeps the whole entry, so ref/file/digest/sha/installedAt survive.
    const QJsonValue entry = m_data.take(oldName);
    m_data.insert(newName, entry);
    save();
    emit changed(oldName);
    emit changed(newName);
}

int Provenance::rebasePaths(const QString &from, const QString &to)
{
    if (from.isEmpty() || from == to)
        return 0;
    const QString prefix = QDir(from).absolutePath() + QLatin1Char('/');
    const QString target = QDir(to).absolutePath() + QLatin1Char('/');
    int rewritten = 0;
    QStringList touched;
    const QJsonObject before = m_data;  // the file is still untouched until save()
    for (auto it = m_data.begin(); it != m_data.end(); ++it) {
        QJsonObject obj = it.value().toObject();
        bool dirty = false;
        for (auto field = obj.begin(); field != obj.end(); ++field) {
            const QString value = field.value().toString();
            if (!value.startsWith(prefix))
                continue;
            field.value() = target + value.mid(prefix.size());
            dirty = true;
            ++rewritten;
        }
        if (dirty) {
            it.value() = obj;
            touched << it.key();
        }
    }
    if (rewritten > 0) {
        // All or nothing: models.json must never be left pointing at a half
        // moved directory, so a failed write drops the rewrite from memory too.
        if (!save()) {
            m_data = before;
            return -1;
        }
        for (const QString &name : std::as_const(touched))
            emit changed(name);
    }
    return rewritten;
}
