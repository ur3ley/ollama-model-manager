#ifndef APPPATHS_H
#define APPPATHS_H

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

/**
 * Where the application keeps its own files (A-4).
 *
 * One root for everything the user may want to inspect or back up:
 *   ~/.config/ollama-manager/config.ini   proxy + UI settings
 *   ~/.config/ollama-manager/models.json  provenance (moved from the old
 *                                          "OllamaManager/Ollama Model Manager")
 *   ~/.config/ollama-manager/tls/         proxy TLS material
 *
 * The directory holds the network topology and the download history, so it
 * is created owner-only (0700) like `tls/` (S-8).
 *
 * Downloaded GGUF sources live in ~/.local/share/ollama-manager/models: the
 * path goes into `FROM` lines of generated Modelfiles, and the old
 * AppDataLocation ("OllamaManager/Ollama Model Manager") had spaces in it.
 * migrateDownloads() moves an old directory once, at startup.
 *
 * Header-only on purpose: the self-tests compile the modules they need one by
 * one and must not grow an extra source file for a few path helpers.
 */
namespace apppaths {

/** ~/.config/ollama-manager (GenericConfigLocation). */
inline QString configDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(base.isEmpty() ? QDir::homePath() : base)
            .filePath(QStringLiteral("ollama-manager"));
}

/** ~/.config/ollama-manager/config.ini */
inline QString configFile()
{
    return QDir(configDir()).filePath(QStringLiteral("config.ini"));
}

/** Create `path` if needed and make it 0700 (also tightens an existing one). */
inline bool ensurePrivateDir(const QString &path)
{
    // A symlinked config dir is the user's own arrangement: leave it alone
    // rather than chmod whatever it points to.
    if (QFileInfo(path).isSymLink())
        return false;
    if (!QDir().mkpath(path))
        return false;
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                               | QFileDevice::ExeOwner);
}

/** Owner-only read/write (0600) for a settings/state file that exists. */
inline void restrictFile(const QString &path)
{
    if (QFile::exists(path))
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

/**
 * Downloaded GGUF sources (HF imports): `~/.local/share/ollama-manager/models`
 * (GenericDataLocation), falling back to the home directory when no data
 * location is available. No spaces: the path ends up in `FROM` lines.
 */
inline QString downloadDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(base.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : base)
            .filePath(QStringLiteral("ollama-manager/models"));
}

/**
 * Where versions before 2026-11-22 kept downloads: AppDataLocation of the app
 * named "Ollama Model Manager" by "OllamaManager". Spelled out instead of asking
 * QStandardPaths, so that it does not depend on the application name.
 */
inline QString legacyDownloadDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(base.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : base)
            .filePath(QStringLiteral("OllamaManager/Ollama Model Manager/models"));
}

/** Result of migrateDownloads(), for the startup log. */
struct DownloadMigration {
    int moved = 0;          // files renamed into the new directory
    QStringList skipped;    // left in place: name taken in the target, or rename failed
};

/**
 * Moves every file of `from` (finished GGUFs and `.part` remainders alike) to
 * `to`, once. Only rename(2): the directories normally share a filesystem, and
 * copying tens of gigabytes at startup would freeze the app. A file whose name
 * is already taken in `to`, or that cannot be renamed, stays where it is and is
 * reported. Empty source directories are removed afterwards (up to, but not
 * including, `stopAt`). Public for the self-test.
 */
inline DownloadMigration migrateDownloads(const QString &from, const QString &to,
                                          const QString &stopAt = QString())
{
    DownloadMigration result;
    const QDir source(from);
    if (from.isEmpty() || QDir(from).absolutePath() == QDir(to).absolutePath() || !source.exists())
        return result;
    const QStringList files = source.entryList(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    if (!files.isEmpty() && !QDir().mkpath(to)) {
        result.skipped = files;
        return result;
    }
    const QDir target(to);
    for (const QString &name : files) {
        const QString destination = target.filePath(name);
        if (QFileInfo::exists(destination) || !QFile::rename(source.filePath(name), destination)) {
            result.skipped << name;
            continue;
        }
        ++result.moved;
    }
    // Drop the emptied legacy tree: models/, "Ollama Model Manager/", "OllamaManager/".
    QDir dir(from);
    const QString stop = stopAt.isEmpty() ? QString() : QDir(stopAt).absolutePath();
    while (dir.exists() && dir.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)
           && dir.absolutePath() != stop) {
        const QString name = dir.dirName();
        if (!dir.cdUp() || !dir.rmdir(name))
            break;
    }
    return result;
}

} // namespace apppaths

#endif // APPPATHS_H
