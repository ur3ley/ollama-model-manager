#ifndef UISETTINGS_H
#define UISETTINGS_H

#include "apppaths.h"

#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QVariant>

/**
 * Interface preferences in `[ui]` of ~/.config/ollama-manager/config.ini, next
 * to the theme (A-4). Until 2026-11-22 the language and the tray hint flag
 * lived in the default QSettings file
 * (~/.config/OllamaManager/Ollama Model Manager.conf); migrateLegacy() copies
 * them over once and leaves the old file untouched, so an older build still
 * finds its own values.
 *
 * Header-only, like apppaths.h.
 */
namespace uisettings {

inline QString languageKey() { return QStringLiteral("ui/language"); }
inline QString trayHintKey() { return QStringLiteral("ui/tray_close_hint_shown"); }

/** The pre-2026-11-22 QSettings file (organization/application names spelled out). */
inline QString legacyFile()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(base.isEmpty() ? QDir::homePath() + QStringLiteral("/.config") : base)
            .filePath(QStringLiteral("OllamaManager/Ollama Model Manager.conf"));
}

inline void setValue(const QString &key, const QVariant &value)
{
    apppaths::ensurePrivateDir(apppaths::configDir());
    QSettings ini(apppaths::configFile(), QSettings::IniFormat);
    ini.setValue(key, value);
    ini.sync();
    apppaths::restrictFile(apppaths::configFile());
}

/**
 * Copies `language` and `trayCloseHintShown` from `legacyPath` into
 * `configPath` unless the new keys are already set. Returns the number of
 * values copied. Paths are parameters for the self-test.
 */
inline int migrateLegacy(const QString &legacyPath = legacyFile(),
                         const QString &configPath = apppaths::configFile())
{
    if (!QFileInfo::exists(legacyPath))
        return 0;
    QSettings legacy(legacyPath, QSettings::IniFormat);
    QSettings ini(configPath, QSettings::IniFormat);
    int copied = 0;
    const QString oldLanguage = legacy.value(QStringLiteral("language")).toString().trimmed();
    if (!ini.contains(languageKey()) && !oldLanguage.isEmpty()) {
        ini.setValue(languageKey(), oldLanguage);
        ++copied;
    }
    if (!ini.contains(trayHintKey()) && legacy.contains(QStringLiteral("trayCloseHintShown"))) {
        ini.setValue(trayHintKey(), legacy.value(QStringLiteral("trayCloseHintShown")).toBool());
        ++copied;
    }
    if (copied > 0) {
        ini.sync();
        apppaths::restrictFile(configPath);
    }
    return copied;
}

inline QString language()
{
    return QSettings(apppaths::configFile(), QSettings::IniFormat)
            .value(languageKey(), QStringLiteral("en")).toString();
}

inline void setLanguage(const QString &code) { setValue(languageKey(), code); }

inline bool trayHintShown()
{
    return QSettings(apppaths::configFile(), QSettings::IniFormat)
            .value(trayHintKey(), false).toBool();
}

inline void setTrayHintShown() { setValue(trayHintKey(), true); }

} // namespace uisettings

#endif // UISETTINGS_H
