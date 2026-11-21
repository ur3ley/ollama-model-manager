#include "carddelegate.h"

#include "designtokens.h"

#include <QFontMetrics>
#include <QHash>
#include <QPainter>

namespace {

QColor avatarColor(const QString &seed)
{
    const int hue = int(qHash(seed) % 360);
    return QColor::fromHsv(hue, 110, 190);
}

} // namespace

CardDelegate::CardDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize CardDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(0, 68);
}

void CardDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect card = option.rect.adjusted(4, 3, -4, -3);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    const QColor background = selected ? QColor(tokens::CardSelected)
                                       : hovered ? QColor(tokens::CardHover)
                                                 : QColor(tokens::Card);
    painter->setPen(QPen(selected ? QColor(tokens::Accent) : QColor(tokens::Border),
                         selected ? 1.6 : 1.0));
    painter->setBrush(background);
    painter->drawRoundedRect(card, tokens::CardRadius, tokens::CardRadius);

    const QString title = index.data(cardrole::Title).toString();
    const QString subtitle = index.data(cardrole::Subtitle).toString();
    const QString stats = index.data(cardrole::Stats).toString();

    const QRect avatar(card.left() + 10, card.center().y() - 15, 30, 30);
    painter->setPen(Qt::NoPen);
    painter->setBrush(avatarColor(subtitle + title));
    painter->drawEllipse(avatar);
    QFont avatarFont = option.font;
    avatarFont.setBold(true);
    painter->setFont(avatarFont);
    painter->setPen(Qt::white);
    painter->drawText(avatar, Qt::AlignCenter, title.left(1).toUpper());

    QFont statsFont = option.font;
    statsFont.setPointSizeF(qMax(7.0, statsFont.pointSizeF() - 0.5));
    const QFontMetrics statsMetrics(statsFont);
    const int statsWidth = stats.isEmpty()
            ? 0
            : qMin(130, statsMetrics.horizontalAdvance(stats) + 14);
    const QRect textRect = card.adjusted(50, 8, -statsWidth - 12, -8);

    QFont titleFont = option.font;
    titleFont.setBold(true);
    painter->setFont(titleFont);
    painter->setPen(QColor(tokens::Text));
    const QFontMetrics titleMetrics(titleFont);
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignTop,
                      titleMetrics.elidedText(title, Qt::ElideRight, textRect.width()));

    QFont subtitleFont = option.font;
    subtitleFont.setPointSizeF(qMax(7.0, subtitleFont.pointSizeF() - 1.0));
    painter->setFont(subtitleFont);
    painter->setPen(QColor(tokens::Muted));
    const QFontMetrics subtitleMetrics(subtitleFont);
    painter->drawText(textRect.adjusted(0, 20, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                      subtitleMetrics.elidedText(subtitle, Qt::ElideRight, textRect.width()));

    if (!stats.isEmpty()) {
        painter->setFont(statsFont);
        painter->setPen(QColor(tokens::Muted));
        painter->drawText(card.adjusted(card.width() - statsWidth - 8, 0, -12, 0),
                          Qt::AlignRight | Qt::AlignVCenter, stats);
    }
    painter->restore();
}
