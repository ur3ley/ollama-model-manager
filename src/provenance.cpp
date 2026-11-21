#include "provenance.h"

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
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return QDir(dir.isEmpty() ? QDir::homePath() : dir).filePath(QStringLiteral("models.json"));
}

Provenance::Provenance()
{
    load();
}

void Provenance::load()
{
    QFile file(storePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isObject())
        m_data = doc.object();
}

void Provenance::save() const
{
    const QString path = storePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(m_data).toJson(QJsonDocument::Indented));
    file.commit();
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
}
