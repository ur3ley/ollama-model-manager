#ifndef DOTICONS_H
#define DOTICONS_H

#include <QColor>
#include <QIcon>
#include <QPainter>
#include <QPixmap>

// Small round status dot: green = fits / ok, red = does not fit.
inline QIcon dotIcon(bool fits)
{
    static QIcon green;
    static QIcon red;
    auto make = [](const QColor &color) {
        QPixmap pixmap(10, 10);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(1, 1, 8, 8);
        return QIcon(pixmap);
    };
    if (fits) {
        if (green.isNull())
            green = make(QColor(QStringLiteral("#16a34a")));
        return green;
    }
    if (red.isNull())
        red = make(QColor(QStringLiteral("#dc2626")));
    return red;
}

#endif // DOTICONS_H
