#ifndef THEME_H
#define THEME_H

#include <QColor>
#include <QObject>
#include <QString>

// Appearance engine: one active palette, three modes (system / light / dark)
// and the global style sheet built from that palette. Everything that paints
// or styles a widget asks theme::palette() instead of hard-coding a colour.
namespace theme {

enum class Mode { System, Light, Dark };

struct Palette {
    QColor bg;           // window background
    QColor surface;      // panels, input fields
    QColor surfaceAlt;   // secondary blocks, table headers
    QColor popup;        // menus, combo popups: a layer above the card
    QColor card;         // list card
    QColor cardHover;    // hovered card
    QColor selected;     // selected card
    QColor text;         // primary text
    QColor muted;        // captions
    QColor faint;        // tertiary text, placeholders
    QColor border;       // borders
    QColor borderStrong; // hover / focus border
    QColor accent;       // accent text, icons, selection outline
    QColor accentHover;  // hovered accent button
    QColor accentFill;   // primary button fill
    QColor accentSoft;   // soft accent backdrop (active tab, badges)
    QColor onAccent;     // text on accentFill
    QColor success;
    QColor warning;
    QColor danger;

    int cardRadius = 12;
    int controlRadius = 8;
    int pillRadius = 999;

    bool isDark = false;
};

Palette lightPalette();
Palette darkPalette();

class Theme : public QObject
{
    Q_OBJECT
public:
    static Theme *instance();

    Mode mode() const { return m_mode; }
    /** Applies the theme immediately and persists it (unless OMM_THEME is set). */
    void setMode(Mode mode);

    bool isDark() const { return m_palette.isDark; }
    const Palette &palette() const { return m_palette; }

    /** Mode actually in effect (System resolved through the desktop hint). */
    Mode effectiveMode() const;

    static QString modeToString(Mode mode);
    static Mode modeFromString(const QString &text, Mode fallback = Mode::System);

    /** Pushes the current palette and style sheet onto the QApplication. */
    void install();

signals:
    /** Palette or mode changed: repaint custom-painted views and restyle. */
    void changed();

private:
    explicit Theme(QObject *parent = nullptr);

    void resolvePalette();
    void onColorSchemeChanged();
    void loadMode();

    Mode m_mode = Mode::System;
    Palette m_palette;
    // OMM_THEME pins the theme for this run (screenshots, tests): switching is
    // allowed but never written back to the config.
    bool m_envOverride = false;
};

/** Global style sheet built from the active palette. */
QString styleSheet();

/**
 * Installs Fusion + the palette + the global style sheet. Call once from
 * main() right after QApplication is created, before any window exists.
 * No-op when the application object is not a QApplication (headless run).
 */
void apply();

} // namespace theme

#endif // THEME_H
