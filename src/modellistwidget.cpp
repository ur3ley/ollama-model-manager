#include "modellistwidget.h"

#include <QTimer>

#include "carddelegate.h"
#include "memoryinfo.h"
#include "format.h"
#include "provenance.h"
#include "theme.h"

#include <QApplication>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QListWidget>
#include <QMenu>
#include <QSignalBlocker>
#include <QStyle>

ModelListWidget::ModelListWidget(OllamaCli *cli, QWidget *parent)
    : QWidget(parent), m_cli(cli)
{
    setupUI();
    // create/delete/copy/rename results refresh the list through
    // MainWindow::onRefreshAll(); a second subscription here only doubled the
    // `ollama list` runs. Errors are shown by the main window's status bar.
    connect(m_cli, &OllamaCli::modelsListed, this,
            [this](bool ok, const QList<OllamaCli::ModelInfo> &models, const QString &) {
        m_refreshing = false;
        m_refreshBtn->setEnabled(true);
        m_refreshBtn->setText(T("list.refresh"));
        if (ok)
            populateList(models);
        if (m_refreshAgain) {
            m_refreshAgain = false;
            refresh();
        }
    });
    // A source bound, a model renamed or removed: the cards group by
    // provenance, so regroup the list we have (A-3). A list request in flight
    // regroups anyway — and an operation that changed provenance usually
    // starts one within the same turn, hence the short delay before checking.
    connect(&Provenance::instance(), &Provenance::changed, this, [this](const QString &) {
        if (m_regroupQueued || m_refreshing)
            return;
        m_regroupQueued = true;
        QTimer::singleShot(150, this, [this]() {
            m_regroupQueued = false;
            if (m_refreshing || m_models.isEmpty())
                return;
            m_groups = groupInstalledModels(m_models, Provenance::instance());
            applyGroups(false);
        });
    });
}

void ModelListWidget::setupUI()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *headerLayout = new QHBoxLayout();
    m_title = new QLabel();
    QFont boldFont = m_title->font();
    boldFont.setBold(true);
    boldFont.setPointSize(boldFont.pointSize() + 2);
    m_title->setFont(boldFont);
    headerLayout->addWidget(m_title);
    headerLayout->addStretch();
    m_refreshBtn = new QPushButton();
    headerLayout->addWidget(m_refreshBtn);
    layout->addLayout(headerLayout);

    m_filterEdit = new QLineEdit();
    m_filterEdit->setClearButtonEnabled(true);
    layout->addWidget(m_filterEdit);

    m_list = new QListWidget();
    m_list->setObjectName(QStringLiteral("modelList"));
    m_list->setMinimumWidth(330);
    m_list->setItemDelegate(new CardDelegate(m_list));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setUniformItemSizes(true);
    m_list->setMouseTracking(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_list, 1);

    connect(m_refreshBtn, &QPushButton::clicked, this, &ModelListWidget::refresh);
    connect(m_list, &QListWidget::customContextMenuRequested, this, &ModelListWidget::onContextMenu);
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() {
        const QString id = currentGroupId();
        if (!id.isEmpty())
            emit repoSelected(id);
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, &ModelListWidget::onFilterChanged);

    // The buttons take their look from the global theme sheet; the card list is
    // painted by CardDelegate, which must repaint when the palette changes.
    connect(theme::Theme::instance(), &theme::Theme::changed, this, [this]() {
        m_list->viewport()->update();
    });

    retranslateUi();
    connect(I18n::instance(), &I18n::languageChanged, this, &ModelListWidget::retranslateUi);
}

void ModelListWidget::retranslateUi()
{
    m_title->setText(T("list.title"));
    m_refreshBtn->setText(T("list.refresh"));
    m_refreshBtn->setToolTip(T("list.refreshTip"));
    m_filterEdit->setPlaceholderText(T("list.filter"));

    // Subtitle/stats are painted by the delegate from the item data, so the
    // visible language only changes after the cards are re-applied. No CLI call:
    // the groups are already in memory.
    if (!m_groups.isEmpty())
        applyGroups(false);
}

void ModelListWidget::refresh()
{
    if (m_refreshing) {
        m_refreshAgain = true;
        return;
    }
    m_refreshing = true;
    m_refreshBtn->setEnabled(false);
    m_refreshBtn->setText(T("list.refreshing"));
    m_cli->listModelsAsync();
}

void ModelListWidget::populateList(const QList<OllamaCli::ModelInfo> &models)
{
    m_models = models;
    m_groups = groupInstalledModels(models, Provenance::instance());
    applyGroups(true);
    emit modelsLoaded();
}

void ModelListWidget::applyGroups(bool notify)
{
    const QString previous = currentGroupId();

    {
        const QSignalBlocker blocker(m_list);
        m_list->clear();
        for (const RepoGroup &group : std::as_const(m_groups)) {
            auto *item = new QListWidgetItem(m_list);
            item->setText(group.title);
            item->setData(cardrole::Id, group.id);
            item->setData(cardrole::Title, group.title);
            item->setData(cardrole::Subtitle, subtitleFor(group));
            item->setData(cardrole::Stats, statsFor(group));
            QString tip = group.ref.isEmpty() ? group.title : group.ref;
            tip += QLatin1Char('\n') + T("list.quantCount").arg(group.quants.size());
            item->setToolTip(tip);
        }
    }

    if (!previous.isEmpty())
        selectRepo(previous);
    if (m_list->count() > 0 && !m_list->currentItem())
        m_list->setCurrentRow(0);
    onFilterChanged(m_filterEdit->text());

    // The selection was restored with signals blocked (no reload for a pure
    // relabel), but a refresh after create/rename/delete means the card content
    // changed: tell the editor to reload the repository it is showing.
    const QString current = currentGroupId();
    if (notify && !current.isEmpty())
        emit repoSelected(current);
}

QString ModelListWidget::subtitleFor(const RepoGroup &group) const
{
    if (group.local)
        return T("list.localSource");
    // A single quant is worth naming: for HuggingFace that is the quantization
    // (IQ4_XS-MTP) plus the current name after a rename, for Ollama a renamed
    // model's current name.
    if (group.quants.size() == 1) {
        const InstalledQuant &quant = group.quants.first();
        if (!quant.file.isEmpty()) {
            QString line = group.owner;
            if (!quant.quantLabel.isEmpty())
                line += QStringLiteral(" · ") + quant.quantLabel;
            if (quant.renamed())
                line += QStringLiteral(" · ") + quant.modelName;
            return line;
        }
        if (quant.renamed())
            return group.owner + QStringLiteral(" · ") + quant.modelName;
    }
    return group.owner.isEmpty() ? group.ref : group.owner;
}

QString ModelListWidget::statsFor(const RepoGroup &group) const
{
    if (group.quants.isEmpty())
        return QString();
    if (group.quants.size() == 1)
        return group.quants.first().size;

    quint64 total = 0;
    bool known = false;
    for (const InstalledQuant &quant : group.quants) {
        const quint64 bytes = memoryinfo::parseSizeBytes(quant.size);
        if (bytes > 0) {
            total += bytes;
            known = true;
        }
    }
    const QString count = QString::number(group.quants.size());
    return known ? count + QStringLiteral(" · ") + format::byteSize(qint64(total)) : count;
}

QString ModelListWidget::currentGroupId() const
{
    QListWidgetItem *item = m_list->currentItem();
    return item ? item->data(cardrole::Id).toString() : QString();
}

QString ModelListWidget::currentModel() const
{
    const RepoGroup group = this->group(currentGroupId());
    return group.quants.isEmpty() ? QString() : group.quants.first().modelName;
}

InstalledSummary ModelListWidget::summary() const
{
    InstalledSummary summary;
    summary.repositories = m_groups.size();
    for (const RepoGroup &group : m_groups) {
        summary.quants += group.quants.size();
        for (const InstalledQuant &quant : group.quants)
            summary.bytes += memoryinfo::parseSizeBytes(quant.size);
    }
    return summary;
}

QStringList ModelListWidget::modelNames() const
{
    QStringList names;
    for (const RepoGroup &group : m_groups) {
        for (const InstalledQuant &quant : group.quants)
            names << quant.modelName;
    }
    return names;
}

RepoGroup ModelListWidget::group(const QString &id) const
{
    for (const RepoGroup &group : m_groups) {
        if (group.id == id)
            return group;
    }
    return RepoGroup();
}

RepoGroup ModelListWidget::groupOfModel(const QString &modelName) const
{
    for (const RepoGroup &group : m_groups) {
        for (const InstalledQuant &quant : group.quants) {
            if (quant.modelName == modelName)
                return group;
        }
    }
    return RepoGroup();
}

void ModelListWidget::selectRepo(const QString &id)
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (item->data(cardrole::Id).toString() != id)
            continue;
        const QSignalBlocker blocker(m_list);
        m_list->setCurrentItem(item);
        m_list->scrollToItem(item);
        return;
    }
}

void ModelListWidget::onContextMenu(QPoint pos)
{
    QListWidgetItem *item = m_list->itemAt(pos);
    if (!item)
        return;

    const QString id = item->data(cardrole::Id).toString();
    const RepoGroup repo = group(id);
    if (repo.quants.isEmpty())
        return;

    QMenu menu(this);
    QAction *editAction = menu.addAction(T("ctx.edit"));
    QAction *bindAction = nullptr;
    if (repo.local && !repo.quants.isEmpty())
        bindAction = menu.addAction(T("bind.open"));

    QHash<QAction *, QString> renameActions;
    QHash<QAction *, QString> deleteActions;

    if (repo.quants.size() == 1) {
        const QString name = repo.quants.first().modelName;
        renameActions.insert(menu.addAction(T("ctx.rename")), name);
        menu.addSeparator();
        QAction *del = menu.addAction(T("ctx.delete"));
        del->setIcon(QApplication::style()->standardIcon(QStyle::SP_TrashIcon));
        deleteActions.insert(del, name);
    } else {
        // Several quants in one card: the action needs to say which one.
        QMenu *renameMenu = menu.addMenu(T("ctx.renameQuant"));
        QMenu *deleteMenu = menu.addMenu(T("ctx.deleteQuant"));
        for (const InstalledQuant &quant : repo.quants) {
            renameActions.insert(renameMenu->addAction(quant.modelName), quant.modelName);
            deleteActions.insert(deleteMenu->addAction(quant.modelName), quant.modelName);
        }
    }

    QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (!chosen)
        return;
    if (chosen == editAction) {
        const QSignalBlocker blocker(m_list);
        m_list->setCurrentItem(item);
        emit repoSelected(id);
        return;
    }
    if (bindAction && chosen == bindAction) {
        emit bindSourceRequested(repo.quants.first().modelName);
        return;
    }
    if (renameActions.contains(chosen))
        emit renameRequested(renameActions.value(chosen));
    else if (deleteActions.contains(chosen))
        emit deleteRequested(deleteActions.value(chosen));
}

void ModelListWidget::onFilterChanged(const QString &text)
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        const RepoGroup repo = group(item->data(cardrole::Id).toString());
        const bool matches = text.isEmpty()
                || repo.searchText().contains(text, Qt::CaseInsensitive);
        item->setHidden(!matches);
    }
}
