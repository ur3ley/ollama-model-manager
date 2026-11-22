#include "downloadsdialog.h"

#include "discoverpanel.h"
#include "format.h"
#include "i18n.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QDateTime>
#include <QMessageBox>
#include <QMouseEvent>
#include <QProcess>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

QString pullStatus(const OllamaCli::PullInfo &pull)
{
    if (!pull.active)
        return T(QStringLiteral("downloads.queued"));
    return pull.percent >= 0
            ? T(QStringLiteral("downloads.runningPercent")).arg(pull.percent)
            : T(QStringLiteral("downloads.running"));
}

QString hfStatus(const DiscoverPanel::HfTaskInfo &task)
{
    if (task.creating)
        return T(QStringLiteral("downloads.creating"));
    return task.active ? T(QStringLiteral("downloads.running"))
                       : T(QStringLiteral("downloads.queued"));
}

void clearRows(QVBoxLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

} // namespace

DownloadsDialog::DownloadsDialog(OllamaCli *cli, DiscoverPanel *hfPanel, bool ollama,
                                 QWidget *parent)
    : QDialog(parent)
    , m_cli(cli)
    , m_hf(hfPanel)
    , m_ollama(ollama)
{
    // A browser-style popup: closes on an outside click, floats above the
    // counter instead of stealing the window.
    setWindowFlags(Qt::Popup);
    setObjectName(QStringLiteral("downloadsDialog"));
    setMinimumWidth(420);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 12, 14, 12);
    outer->setSpacing(10);

    auto *title = new QLabel(T(m_ollama ? QStringLiteral("downloads.titleOllama")
                                        : QStringLiteral("downloads.titleHf")), this);
    title->setProperty("section", true);
    outer->addWidget(title);

    if (m_ollama) {
        m_pullRows = new QVBoxLayout();
        m_pullRows->setContentsMargins(0, 0, 0, 0);
        m_pullRows->setSpacing(4);
        outer->addLayout(m_pullRows);
        m_pullEmpty = new QLabel(T(QStringLiteral("downloads.empty")), this);
        m_pullEmpty->setProperty("muted", true);
        outer->addWidget(m_pullEmpty);

    } else {
        m_hfRows = new QVBoxLayout();
        m_hfRows->setContentsMargins(0, 0, 0, 0);
        m_hfRows->setSpacing(4);
        outer->addLayout(m_hfRows);
        m_hfEmpty = new QLabel(T(QStringLiteral("downloads.empty")), this);
        m_hfEmpty->setProperty("muted", true);
        outer->addWidget(m_hfEmpty);

        m_incTitle = new QLabel(T(QStringLiteral("downloads.incompleteTitle")), this);
        m_incTitle->setProperty("section", true);
        m_incTitle->setVisible(false);
        outer->addWidget(m_incTitle);
        m_incRows = new QVBoxLayout();
        m_incRows->setContentsMargins(0, 0, 0, 0);
        m_incRows->setSpacing(4);
        outer->addLayout(m_incRows);
    }
    outer->addStretch(1);

    m_timer = new QTimer(this);
    m_timer->setInterval(500);
    connect(m_timer, &QTimer::timeout, this, &DownloadsDialog::refresh);
    refresh();
}

void DownloadsDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    refresh();
    m_timer->start();
}

void DownloadsDialog::hideEvent(QHideEvent *event)
{
    m_timer->stop();
    QDialog::hideEvent(event);
}

void DownloadsDialog::rebuildPulls()
{
    clearRows(m_pullRows);
    m_rows.clear();
    m_rowIds.clear();
    m_order.clear();

    const QList<OllamaCli::PullInfo> pulls = m_cli->pullList();
    m_activeId = (!pulls.isEmpty() && pulls.first().active) ? pulls.first().id : 0;
    for (int i = 0; i < pulls.size(); ++i) {
        const OllamaCli::PullInfo pull = pulls.at(i);
        m_order.append(pull.id);

        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        auto *name = new QLabel(pull.name, row);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *status = new QLabel(pullStatus(pull), row);
        status->setProperty("muted", true);
        status->setAttribute(Qt::WA_TransparentForMouseEvents);

        auto *up = new QToolButton(row);
        up->setText(QStringLiteral("\u2191"));
        up->setToolTip(T(QStringLiteral("downloads.moveUp")));
        up->setVisible(!pull.active);
        up->setEnabled(!pull.active && i > 1);

        auto *down = new QToolButton(row);
        down->setText(QStringLiteral("\u2193"));
        down->setToolTip(T(QStringLiteral("downloads.moveDown")));
        down->setVisible(!pull.active);
        down->setEnabled(!pull.active && i < pulls.size() - 1);

        auto *cancel = new QToolButton(row);
        cancel->setText(T(QStringLiteral("downloads.cancel")));

        layout->addWidget(name, 1);
        layout->addWidget(status);
        layout->addWidget(up);
        layout->addWidget(down);
        layout->addWidget(cancel);

        const quint64 id = pull.id;
        connect(cancel, &QToolButton::clicked, this, [this, id]() {
            m_cli->cancelPull(id);
            refresh();
        });
        connect(up, &QToolButton::clicked, this, [this, id]() {
            m_cli->movePull(id, -1);
            refresh();
        });
        connect(down, &QToolButton::clicked, this, [this, id]() {
            m_cli->movePull(id, 1);
            refresh();
        });

        m_rows.insert(id, PullRow{row, status, up, down});
        m_pullRows->addWidget(row);

        row->setAcceptDrops(true);
        row->setAttribute(Qt::WA_StyledBackground, true);
        row->installEventFilter(this);
        m_rowIds.insert(row, id);
    }

    m_pullEmpty->setVisible(pulls.isEmpty());
}

void DownloadsDialog::rebuildHf()
{
    clearRows(m_hfRows);
    m_hfRowList.clear();
    m_hfOrder.clear();
    m_hfRowIds.clear();
    m_hfActiveId.clear();

    const QList<DiscoverPanel::HfTaskInfo> tasks = m_hf->hfTasks();
    if (!tasks.isEmpty() && tasks.first().active)
        m_hfActiveId = tasks.first().id;
    for (const DiscoverPanel::HfTaskInfo &task : tasks) {
        m_hfOrder.append(task.id);

        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(6);

        auto *name = new QLabel(task.label, row);
        name->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto *status = new QLabel(hfStatus(task), row);
        status->setProperty("muted", true);
        status->setAttribute(Qt::WA_TransparentForMouseEvents);

        auto *cancel = new QToolButton(row);
        cancel->setText(T(QStringLiteral("downloads.cancel")));
        cancel->setEnabled(task.cancellable);
        const QString id = task.id;
        connect(cancel, &QToolButton::clicked, this, [this, id]() {
            m_hf->cancelHfTask(id);
            refresh();
        });

        layout->addWidget(name, 1);
        layout->addWidget(status);
        layout->addWidget(cancel);
        m_hfRows->addWidget(row);
        m_hfRowList.append(HfRow{row, task.id, status, cancel});

        row->setAttribute(Qt::WA_StyledBackground, true);
        row->installEventFilter(this);
        m_hfRowIds.insert(row, task.id);
    }

    m_hfEmpty->setVisible(tasks.isEmpty());
}

void DownloadsDialog::refresh()
{
    if (!m_ollama) {
        const QList<DiscoverPanel::HfTaskInfo> tasks = m_hf->hfTasks();
        QStringList ids;
        ids.reserve(tasks.size());
        for (const DiscoverPanel::HfTaskInfo &task : tasks)
            ids.append(task.id);
        if (ids != m_hfOrder) {
            rebuildHf();
        } else {
            for (int i = 0; i < tasks.size(); ++i) {
                const DiscoverPanel::HfTaskInfo &task = tasks.at(i);
                if (i >= m_hfRowList.size())
                    break;
                HfRow &row = m_hfRowList[i];
                row.status->setText(hfStatus(task));
                if (row.cancel)
                    row.cancel->setEnabled(task.cancellable);
            }
            m_hfEmpty->setVisible(tasks.isEmpty());
        }
        const QList<DiscoverPanel::HfIncomplete> incomplete = m_hf->hfIncomplete();
        QStringList sidecars;
        sidecars.reserve(incomplete.size());
        for (const DiscoverPanel::HfIncomplete &item : incomplete)
            sidecars.append(item.sidecar);
        if (sidecars != m_incOrder) {
            m_incOrder = sidecars;
            clearRows(m_incRows);
            m_incTitle->setVisible(!incomplete.isEmpty());
            for (const DiscoverPanel::HfIncomplete &item : incomplete) {
                auto *row = new QWidget(this);
                auto *layout = new QHBoxLayout(row);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(6);
                auto *name = new QLabel(item.label, row);
                name->setAttribute(Qt::WA_TransparentForMouseEvents);
                const int percent = item.total > 0
                        ? int(qBound<qint64>(qint64(0), item.downloaded * 100 / item.total,
                                             qint64(100)))
                        : -1;
                auto *state = new QLabel(
                        percent >= 0
                        ? QStringLiteral("%1 / %2 · %3%").arg(format::byteSize(item.downloaded),
                                                              format::byteSize(item.total)).arg(percent)
                        : format::byteSize(item.downloaded), row);
                state->setProperty("muted", true);
                auto *resume = new QToolButton(row);
                resume->setText(T(QStringLiteral("downloads.resume")));
                resume->setVisible(!item.sidecar.isEmpty());
                auto *remove = new QToolButton(row);
                remove->setText(T(QStringLiteral("downloads.remove")));
                layout->addWidget(name, 1);
                layout->addWidget(state);
                layout->addWidget(resume);
                layout->addWidget(remove);
                const QString sidecar = item.sidecar;
                connect(resume, &QToolButton::clicked, this, [this, sidecar]() {
                    m_hf->resumeHfIncomplete(sidecar);
                    refresh();
                });
                connect(remove, &QToolButton::clicked, this, [this, sidecar, label = item.label]() {
                    if (sidecar.isEmpty())
                        m_hf->removeHfPartial(
                                QDir(m_hf->downloadsDir()).filePath(label + QStringLiteral(".part")));
                    else
                        m_hf->removeHfIncomplete(sidecar);
                    refresh();
                });
                m_incRows->addWidget(row);
            }
        }
        return;
    }

    const QList<OllamaCli::PullInfo> pulls = m_cli->pullList();
    m_activeId = (!pulls.isEmpty() && pulls.first().active) ? pulls.first().id : 0;
    QList<quint64> order;
    order.reserve(pulls.size());
    for (const OllamaCli::PullInfo &pull : pulls)
        order.append(pull.id);

    if (order != m_order) {
        rebuildPulls();
    } else {
        for (int i = 0; i < pulls.size(); ++i) {
            const OllamaCli::PullInfo &pull = pulls.at(i);
            auto row = m_rows.constFind(pull.id);
            if (row == m_rows.constEnd())
                continue;
            row->status->setText(pullStatus(pull));
            if (row->up)
                row->up->setEnabled(!pull.active && i > 1);
            if (row->down)
                row->down->setEnabled(!pull.active && i < pulls.size() - 1);
        }
        m_pullEmpty->setVisible(pulls.isEmpty());
    }
}

void DownloadsDialog::setDropRow(QWidget *row)
{
    if (m_dropRow == row)
        return;
    auto repolish = [](QWidget *widget) {
        if (widget) {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
            widget->update();
        }
    };
    if (m_dropRow) {
        m_dropRow->setProperty("dropTarget", false);
        repolish(m_dropRow);
    }
    m_dropRow = row;
    if (m_dropRow) {
        m_dropRow->setProperty("dropTarget", true);
        repolish(m_dropRow);
    }
}

bool DownloadsDialog::eventFilter(QObject *watched, QEvent *event)
{
    QWidget *widget = qobject_cast<QWidget *>(watched);
    const auto pullIt = m_rowIds.constFind(widget);
    const auto hfIt = m_hfRowIds.constFind(widget);
    if (pullIt == m_rowIds.constEnd() && hfIt == m_hfRowIds.constEnd())
        return QDialog::eventFilter(watched, event);

    // The row under the cursor, found by geometry: during a mouse grab the
    // window system hit test is not reliable (and OS drag-and-drop would close
    // the Qt::Popup window before a drop could happen).
    const auto scan = [](const auto &map, const QPoint &global) -> QWidget * {
        for (auto r = map.cbegin(); r != map.cend(); ++r) {
            QWidget *row = r.key();
            if (!row || !row->isVisible())
                continue;
            if (QRect(row->mapToGlobal(QPoint(0, 0)), row->size()).contains(global))
                return row;
        }
        return nullptr;
    };
    const auto rowAt = [&](const QPoint &global) -> QWidget * {
        if (QWidget *row = scan(m_rowIds, global))
            return row;
        return scan(m_hfRowIds, global);
    };

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        const auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton)
            break;
        m_dragStart = me->position().toPoint();
        m_dragging = false;
        m_dragId = 0;
        m_dragHfId.clear();
        // The running row stays put: it can only be the drop target.
        if (pullIt != m_rowIds.constEnd() && *pullIt != m_activeId) {
            m_dragIsHf = false;
            m_dragId = *pullIt;
        } else if (hfIt != m_hfRowIds.constEnd() && *hfIt != m_hfActiveId) {
            m_dragIsHf = true;
            m_dragHfId = *hfIt;
        }
        break;
    }
    case QEvent::MouseMove: {
        const auto *me = static_cast<QMouseEvent *>(event);
        if (!(me->buttons() & Qt::LeftButton) || (!m_dragId && m_dragHfId.isEmpty()))
            break;
        if (!m_dragging
                && (me->position().toPoint() - m_dragStart).manhattanLength()
                        < QApplication::startDragDistance()) {
            break;
        }
        m_dragging = true;
        setDropRow(rowAt(me->globalPosition().toPoint()));
        break;
    }
    case QEvent::MouseButtonRelease: {
        const auto *me = static_cast<QMouseEvent *>(event);
        const bool wasHf = m_dragIsHf;
        const quint64 pullId = m_dragId;
        const QString hfId = m_dragHfId;
        QWidget *target = m_dragging ? rowAt(me->globalPosition().toPoint()) : nullptr;
        m_dragging = false;
        m_dragId = 0;
        m_dragHfId.clear();
        setDropRow(nullptr);
        if (!target)
            break;

        if (wasHf) {
            const QString targetHf = m_hfRowIds.value(target);
            if (targetHf.isEmpty() || targetHf == hfId)
                break;
            // Dropping on the running import promotes it; the running one is
            // requeued behind and resumes from its partial shards.
            if (targetHf == m_hfActiveId)
                m_hf->prioritizeHfTask(hfId);
            else
                m_hf->moveHfTaskBefore(hfId, targetHf);
        } else {
            const quint64 targetId = m_rowIds.value(target);
            if (!targetId || targetId == pullId)
                break;
            if (targetId == m_activeId)
                m_cli->prioritizePull(pullId);
            else
                m_cli->movePullBefore(pullId, targetId);
        }
        refresh();
        break;
    }
    default:
        break;
    }
    return QDialog::eventFilter(watched, event);
}
