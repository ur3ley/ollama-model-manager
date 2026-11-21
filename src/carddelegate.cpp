#include "carddelegate.h"

#include "designtokens.h"

#include <QFontMetrics>
#include <QHash>
#include <QPainter>

namespace {

QColor avatarColor(const QString &seed, const theme::Palette &p)
{
    const int hue = int(qHash(seed) % 360);
    return QColor::fromHsv(hue, 110, p.isDark ? 165 : 190);
}

} // namespace

CardDelegate::CardDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize CardDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return QSize(0, 72);
}

void CardDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    // The delegate outlives any single theme, so the palette is read on every
    // paint() rather than cached in the constructor.
    const theme::Palette &p = tokens::activePalette();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const QRect card = option.rect.adjusted(4, 4, -4, -4);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    const QColor background = selected ? p.selected : hovered ? p.cardHover : p.card;
    painter->setPen(QPen(selected ? p.accent : p.border, selected ? 1.6 : 1.0));
    painter->setBrush(background);
    painter->drawRoundedRect(card, p.cardRadius, p.cardRadius);

    const QString title = index.data(cardrole::Title).toString();
    const QString subtitle = index.data(cardrole::Subtitle).toString();
    const QString stats = index.data(cardrole::Stats).toString();

    constexpr int avatarSide = 34;
    const QRect avatar(card.left() + 12, card.center().y() - avatarSide / 2,
                       avatarSide, avatarSide);
    painter->setPen(Qt::NoPen);
    painter->setBrush(avatarColor(subtitle + title, p));
    painter->drawEllipse(avatar);
    QFont avatarFont = option.font;
    avatarFont.setPixelSize(13);
    avatarFont.setBold(true);
    painter->setFont(avatarFont);
    painter->setPen(p.onAccent);
    painter->drawText(avatar, Qt::AlignCenter, title.left(1).toUpper());

    QFont statsFont = option.font;
    statsFont.setPixelSize(11);
    const QFontMetrics statsMetrics(statsFont);
    const int statsWidth = stats.isEmpty()
            ? 0
            : qMin(130, statsMetrics.horizontalAdvance(stats) + 14);
    const QRect textRect = card.adjusted(avatarSide + 24, 10, -statsWidth - 12, -10);

    QFont titleFont = option.font;
    titleFont.setPixelSize(13);
    titleFont.setBold(true);
    painter->setFont(titleFont);
    painter->setPen(p.text);
    const QFontMetrics titleMetrics(titleFont);
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignTop,
                      titleMetrics.elidedText(title, Qt::ElideRight, textRect.width()));

    QFont subtitleFont = option.font;
    subtitleFont.setPixelSize(11);
    painter->setFont(subtitleFont);
    painter->setPen(p.muted);
    const QFontMetrics subtitleMetrics(subtitleFont);
    painter->drawText(textRect.adjusted(0, 20, 0, 0), Qt::AlignLeft | Qt::AlignTop,
                      subtitleMetrics.elidedText(subtitle, Qt::ElideRight, textRect.width()));

    if (!stats.isEmpty()) {
        painter->setFont(statsFont);
        painter->setPen(p.muted);
        painter->drawText(card.adjusted(card.width() - statsWidth - 8, 0, -12, 0),
                          Qt::AlignRight | Qt::AlignVCenter, stats);
    }
    painter->restore();
}
