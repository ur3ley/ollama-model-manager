#ifndef CARDDELEGATE_H
#define CARDDELEGATE_H

#include <QStyledItemDelegate>

// Paints a rounded "card" for a model row: avatar circle with the first letter,
// bold title, muted subtitle and right-aligned stats. Shared by the hub source
// panels and the installed-model list so both look the same.
namespace cardrole {
enum Role {
    Id = Qt::UserRole,
    Title,
    Subtitle,
    Stats,
};
}

class CardDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit CardDelegate(QObject *parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};

#endif // CARDDELEGATE_H
