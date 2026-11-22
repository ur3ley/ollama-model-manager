#ifndef DOWNLOADSDIALOG_H
#define DOWNLOADSDIALOG_H

#include <QDialog>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QString>
#include <QStringList>

#include "ollamacli.h"

class DiscoverPanel;
class QLabel;
class QTimer;
class QToolButton;
class QVBoxLayout;

// Browser-like downloads panel behind the global "↓ N" counter. Ollama pulls
// and HuggingFace imports live in two separate columns, each with the running
// task on top and the waiting ones below. Ollama rows can be dragged: dropping
// one onto the running row promotes it (the running pull is requeued behind it).
class DownloadsDialog : public QDialog
{
    Q_OBJECT
public:
    DownloadsDialog(OllamaCli *cli, DiscoverPanel *hfPanel, bool ollama,
                    QWidget *parent = nullptr);

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    void rebuildPulls();
    void rebuildHf();
    void setDropRow(QWidget *row);

    OllamaCli *m_cli;
    DiscoverPanel *m_hf;
    bool m_ollama;
    QVBoxLayout *m_pullRows = nullptr;
    QVBoxLayout *m_hfRows = nullptr;
    QLabel *m_pullEmpty = nullptr;
    QLabel *m_hfEmpty = nullptr;
    QLabel *m_incTitle = nullptr;
    QVBoxLayout *m_incRows = nullptr;
    QStringList m_incOrder;
    QTimer *m_timer = nullptr;

    struct PullRow {
        QWidget *widget = nullptr;
        QLabel *status = nullptr;
        QToolButton *up = nullptr;
        QToolButton *down = nullptr;
    };
    QHash<quint64, PullRow> m_rows;
    QList<quint64> m_order;
    QHash<QWidget *, quint64> m_rowIds;
    QHash<QWidget *, QString> m_hfRowIds;
    QString m_hfActiveId;
    QString m_dragHfId;
    bool m_dragIsHf = false;
    QPoint m_dragStart;
    quint64 m_dragId = 0;
    quint64 m_activeId = 0;
    bool m_dragging = false;
    QWidget *m_dropRow = nullptr;

    struct HfRow {
        QWidget *widget = nullptr;
        QString id;
        QLabel *status = nullptr;
        QToolButton *cancel = nullptr;
    };
    QList<HfRow> m_hfRowList;
    QStringList m_hfOrder;
    QList<bool> m_hfCreating;
};

#endif // DOWNLOADSDIALOG_H
