#ifndef DESIGNTOKENS_H
#define DESIGNTOKENS_H

#include <QColor>
#include <QString>
#include <QStyle>
#include <QWidget>

#include "theme.h"

// Colour and shape helpers for widgets that need their own little style sheet.
// The colours always come from the active theme palette: nothing here (and
// nothing in the callers) hard-codes a hex value, so light/dark stay in sync.
//
// Most widgets do not need these at all — the global sheet installed by
// theme::apply() already styles buttons, fields, tabs and so on. Two property
// flags cover the rest of the old per-widget sheets:
//   label->setProperty("muted", true);      -> QLabel[muted="true"]
//   label->setProperty("caption", true);    -> QLabel[caption="true"]
//   label->setProperty("role", "danger");   -> QLabel[role="danger"]
// (the helpers below also repolish, for properties that change at runtime).
namespace tokens {

inline const theme::Palette &activePalette()
{
    return theme::Theme::instance()->palette();
}

/** Linear mix: t = 0 -> a, t = 1 -> b. */
inline QColor blend(const QColor &a, const QColor &b, qreal t)
{
    const qreal u = 1.0 - t;
    return QColor::fromRgbF(a.redF() * u + b.redF() * t,
                            a.greenF() * u + b.greenF() * t,
                            a.blueF() * u + b.blueF() * t);
}

inline void repolish(QWidget *widget)
{
    if (!widget)
        return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

/** Semantic label colour: "danger", "warning", "success" (or "" to clear). */
inline void setRole(QWidget *widget, const char *role)
{
    if (!widget)
        return;
    widget->setProperty("role", role && *role ? QString::fromLatin1(role) : QVariant());
    repolish(widget);
}

inline void setMuted(QWidget *widget, bool on = true)
{
    if (!widget)
        return;
    widget->setProperty("muted", on);
    repolish(widget);
}

inline void setCaption(QWidget *widget, bool on = true)
{
    if (!widget)
        return;
    widget->setProperty("caption", on);
    repolish(widget);
}

inline QString comboStyle(const theme::Palette &p = activePalette())
{
    return QStringLiteral("QComboBox { background:%1; color:%2; border:1px solid %3;"
                          " border-radius:%4px; padding:6px 10px; }"
                          "QComboBox::drop-down { border:none; width:20px; }"
                          "QComboBox QAbstractItemView { background:%1; color:%2;"
                          " border:1px solid %3; selection-background-color:%5; }")
            .arg(p.surface.name(), p.text.name(), p.border.name())
            .arg(p.controlRadius)
            .arg(p.accentSoft.name());
}

inline QString primaryButtonStyle(const theme::Palette &p = activePalette())
{
    return QStringLiteral("QPushButton { background:%1; color:%2; border:1px solid %1;"
                          " border-radius:%3px; padding:7px 18px; font-weight:600; }"
                          "QPushButton:hover { background:%4; border-color:%4; }"
                          "QPushButton:disabled { background:%5; color:%6;"
                          " border-color:%5; }")
            .arg(p.accentFill.name(), p.onAccent.name())
            .arg(p.controlRadius)
            .arg(p.accentHover.name(), p.accentSoft.name(), p.faint.name());
}

inline QString ghostButtonStyle(const theme::Palette &p = activePalette())
{
    return QStringLiteral("QPushButton { background:%1; color:%2; border:1px solid %3;"
                          " border-radius:%4px; padding:7px 14px; }"
                          "QPushButton:hover { background:%5; }"
                          "QPushButton:disabled { color:%6; border-color:%7; }")
            .arg(p.surface.name(), p.text.name(), p.borderStrong.name())
            .arg(p.controlRadius)
            .arg(p.cardHover.name(), p.faint.name(), p.border.name());
}

inline QString progressStyle(const theme::Palette &p = activePalette())
{
    return QStringLiteral("QProgressBar { border:1px solid %1; border-radius:6px;"
                          " background:%2; color:%3; height:14px; text-align:center;"
                          " font-size:10px; }"
                          "QProgressBar::chunk { background:%4; border-radius:5px; }")
            .arg(p.border.name(), p.surfaceAlt.name(), p.text.name(), p.accentFill.name());
}

inline QString chipStyle(const QString &kind, const theme::Palette &p = activePalette())
{
    QColor hue(QStringLiteral("#6b7280"));
    if (kind == QLatin1String("vision"))
        hue = QColor(QStringLiteral("#6366f1"));
    else if (kind == QLatin1String("thinking"))
        hue = QColor(QStringLiteral("#9333ea"));
    else if (kind == QLatin1String("tools"))
        hue = QColor(QStringLiteral("#2563eb"));
    else if (kind == QLatin1String("embedding"))
        hue = QColor(QStringLiteral("#059669"));
    else if (kind == QLatin1String("gguf"))
        hue = QColor(QStringLiteral("#64748b"));

    // Tint the badge from the hue instead of storing a light/dark pair per kind:
    // one rule works for both palettes (surface with a touch of hue).
    const QColor background = blend(p.isDark ? p.surfaceAlt : p.surface, hue,
                                    p.isDark ? 0.26 : 0.10);
    const QColor foreground = p.isDark ? hue.lighter(142) : hue.darker(118);
    return QStringLiteral("QLabel { background:%1; color:%2; border-radius:%3px;"
                          " padding:2px 9px; font-size:11px; }")
            .arg(background.name(), foreground.name())
            .arg(p.pillRadius);
}

} // namespace tokens

#endif // DESIGNTOKENS_H
