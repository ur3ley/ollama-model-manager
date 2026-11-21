#ifndef POPUPCOMBOBOX_H
#define POPUPCOMBOBOX_H

#include <QAbstractItemView>
#include <QComboBox>
#include <QScreen>
#include <QScrollBar>

// QComboBox whose popup keeps a predictable height. Qt's style-sheet proxy
// reports SH_ComboBox_Popup and then ignores maxVisibleItems, so a two-entry
// list could open one-and-a-half rows high with a scrollbar. This clamps the
// container to `rows * rowHeight + chrome` and keeps it on screen.
class PopupComboBox : public QComboBox
{
public:
    explicit PopupComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        setMaxVisibleItems(kMaxVisibleRows);
        view()->setTextElideMode(Qt::ElideMiddle);
        view()->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    void showPopup() override
    {
        view()->setMinimumWidth(width());
        // Size the list before the container lays itself out: Qt's own height
        // is one row short on some styles, which produces the arrow scrollers.
        const int rows = qMin(count(), kMaxVisibleRows);
        const int rowHeight = qMax(1, view()->sizeHintForRow(0));
        const int wantedView = rows * rowHeight + 2;
        view()->setMinimumHeight(wantedView);
        view()->setMaximumHeight(wantedView);

        QComboBox::showPopup();

        QWidget *popup = view()->window();
        if (!popup)
            return;

        const int chrome = qMax(0, popup->height() - view()->height());
        const int height = wantedView + chrome;
        if (popup->height() != height)
            popup->resize(popup->width(), height);

        const QRect available = screen() ? screen()->availableGeometry() : QRect();
        if (!available.isValid())
            return;

        const QRect comboRect(mapToGlobal(QPoint(0, 0)), size());
        int y = comboRect.bottom() + 1;
        if (y + popup->height() > available.bottom() + 1) {
            const int above = comboRect.top() - popup->height();
            y = above >= available.top()
                    ? above
                    : qMax(available.top(), available.bottom() - popup->height() + 1);
        }
        const int x = qBound(available.left(), comboRect.left(),
                             qMax(available.left(), available.right() - popup->width() + 1));
        popup->move(x, y);
    }

private:
    static constexpr int kMaxVisibleRows = 12;
};

#endif // POPUPCOMBOBOX_H
