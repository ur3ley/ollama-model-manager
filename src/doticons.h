#ifndef DOTICONS_H
#define DOTICONS_H

#include <QColor>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

#include "memoryinfo.h"
#include "theme.h"

// Small round status dot: green = fits / ok, amber = fits but leaves little
// headroom for the KV cache, red = does not fit, grey = unknown or not
// installable. The colours come from the active palette, so the dots stay
// legible in both themes.
inline QIcon dotIcon(memoryinfo::Fit fit)
{
    static QHash<QString, QIcon> cache;
    const theme::Palette &palette = theme::Theme::instance()->palette();
    QColor color = palette.faint;
    switch (fit) {
    case memoryinfo::Fit::Yes:
        color = palette.success;
        break;
    case memoryinfo::Fit::Tight:
        color = palette.warning;
        break;
    case memoryinfo::Fit::No:
        color = palette.danger;
        break;
    case memoryinfo::Fit::Unknown:
        break;
    }

    const auto cached = cache.constFind(color.name());
    if (cached != cache.constEnd())
        return cached.value();

    QPixmap pixmap(10, 10);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(1, 1, 8, 8);
    painter.end();

    const QIcon icon(pixmap);
    cache.insert(color.name(), icon);
    return icon;
}

// Kept for boolean uses such as the Ollama availability dot.
inline QIcon dotIcon(bool ok)
{
    return dotIcon(ok ? memoryinfo::Fit::Yes : memoryinfo::Fit::No);
}

#endif // DOTICONS_H
