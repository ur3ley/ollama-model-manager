#ifndef COMBO_HIGHLIGHT_DELEGATE_H
#define COMBO_HIGHLIGHT_DELEGATE_H

#include <QPainter>
#include <QStyledItemDelegate>

#include "designtokens.h"

// Drop-down rows in a QComboBox popup are painted by the style, and on some
// styles (Fusion with a custom sheet) the hover/current row is either a loud
// system blue or invisible. This delegate draws the same "soft accent card"
// the repository list uses, then lets the base delegate paint the text.
class ComboHighlightDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ComboHighlightDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        const bool hot = opt.state & QStyle::State_MouseOver
                || opt.state & QStyle::State_Selected;
        if (!hot) {
            QStyledItemDelegate::paint(painter, opt, index);
            return;
        }

        const theme::Palette &p = tokens::activePalette();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect row = opt.rect.adjusted(2, 1, -2, -1);
        painter->setPen(QPen(p.accent, 1));
        painter->setBrush(p.selected);
        painter->drawRoundedRect(row, 6, 6);
        painter->restore();

        // No platform selection/hover fill underneath the text.
        opt.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        opt.backgroundBrush = Qt::NoBrush;
        QStyledItemDelegate::paint(painter, opt, index);
    }
};

#endif // COMBO_HIGHLIGHT_DELEGATE_H
