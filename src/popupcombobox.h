#ifndef POPUPCOMBOBOX_H
#define POPUPCOMBOBOX_H

#include <QAbstractItemView>
#include <QComboBox>
#include <QScreen>
#include <QScrollBar>

// QComboBox whose popup keeps a predictable height. Qt's style-sheet proxy
// reports SH_ComboBox_Popup (which bloats the popup and ignores
// maxVisibleItems), but the global sheet turns that mode off. The remaining
// job here is to clamp the container to `rows * rowHeight + border` and keep
// it on screen, so a two-entry list opens exactly two rows high.
class PopupComboBox : public QComboBox
{
    Q_OBJECT
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
        // Size the list before the container lays itself out: otherwise Qt
        // counts only the rows, and the view's 1px border clips the last one.
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
