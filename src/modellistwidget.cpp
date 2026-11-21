#include "modellistwidget.h"
#include "carddelegate.h"
#include "designtokens.h"
#include <QHeaderView>
#include <QListWidget>
#include <QMenu>
#include <QHBoxLayout>
#include <QLabel>
#include <QApplication>
#include <QStyle>

ModelListWidget::ModelListWidget(OllamaCli *cli, QWidget *parent)
    : QWidget(parent), m_cli(cli)
{
    setupUI();
    connect(m_cli, &OllamaCli::createFinished, this, &ModelListWidget::refresh);
    connect(m_cli, &OllamaCli::deleteFinished, this, &ModelListWidget::refresh);
    connect(m_cli, &OllamaCli::copyFinished, this, &ModelListWidget::refresh);
    connect(m_cli, &OllamaCli::modelsListed, this,
            [this](bool ok, const QList<OllamaCli::ModelInfo> &models, const QString &error) {
        Q_UNUSED(error);
        m_refreshing = false;
        m_refreshBtn->setEnabled(true);
        m_refreshBtn->setText(T("list.refresh"));
        if (ok)
            populateList(models);
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

    auto *btnLayout = new QHBoxLayout();
    m_newBtn = new QPushButton();
    btnLayout->addWidget(m_newBtn);
    m_importBtn = new QPushButton();
    btnLayout->addWidget(m_importBtn);
    m_deleteBtn = new QPushButton();
    m_deleteBtn->setEnabled(false);
    btnLayout->addWidget(m_deleteBtn);
    layout->addLayout(btnLayout);

    connect(m_refreshBtn, &QPushButton::clicked, this, &ModelListWidget::refresh);
    connect(m_newBtn, &QPushButton::clicked, this, &ModelListWidget::newModelRequested);
    connect(m_importBtn, &QPushButton::clicked, this, &ModelListWidget::importGGUFRequested);
    connect(m_deleteBtn, &QPushButton::clicked, this, &ModelListWidget::onDeleteClicked);
    connect(m_list, &QListWidget::itemClicked, this, &ModelListWidget::onItemClicked);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &ModelListWidget::onItemDoubleClicked);
    connect(m_list, &QListWidget::customContextMenuRequested, this, &ModelListWidget::onContextMenu);
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() {
        m_deleteBtn->setEnabled(m_list->currentItem() != nullptr);
        if (QListWidgetItem *item = m_list->currentItem())
            emit modelSelected(item->text());
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, &ModelListWidget::onFilterChanged);

    for (QPushButton *button : {m_refreshBtn, m_newBtn, m_importBtn, m_deleteBtn})
        button->setStyleSheet(tokens::ghostButtonStyle());

    retranslateUi();
    connect(I18n::instance(), &I18n::languageChanged, this, &ModelListWidget::retranslateUi);
}

void ModelListWidget::retranslateUi()
{
    m_title->setText(T("list.title"));
    m_refreshBtn->setText(T("list.refresh"));
    m_refreshBtn->setToolTip(T("list.refreshTip"));
    m_filterEdit->setPlaceholderText(T("list.filter"));
    m_newBtn->setText(T("list.new"));
    m_newBtn->setToolTip(T("list.newTip"));
    m_importBtn->setText(T("list.import"));
    m_importBtn->setToolTip(T("list.importTip"));
    m_deleteBtn->setText(T("list.delete"));
    m_deleteBtn->setToolTip(T("list.deleteTip"));
}

void ModelListWidget::refresh()
{
    if (m_refreshing)
        return;
    m_refreshing = true;
    m_refreshBtn->setEnabled(false);
    m_refreshBtn->setText(T("list.refreshing"));
    m_cli->listModelsAsync();
}

void ModelListWidget::populateList(const QList<OllamaCli::ModelInfo> &models)
{
    m_list->clear();

    for (const auto &model : models) {
        auto *item = new QListWidgetItem(m_list);
        item->setText(model.name);
        item->setData(cardrole::Id, model.id);
        item->setData(cardrole::Title, model.name);
        item->setData(cardrole::Subtitle, model.modified);
        item->setData(cardrole::Stats, model.size);
        item->setToolTip(T("list.itemTip").arg(model.id, model.modified));
    }

    if (m_list->count() > 0 && !m_list->currentItem())
        m_list->setCurrentRow(0);
    emit modelsLoaded();
}

QString ModelListWidget::currentModel() const
{
    QListWidgetItem *item = m_list->currentItem();
    return item ? item->text() : QString();
}

QStringList ModelListWidget::modelNames() const
{
    QStringList names;
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        if (!item->isHidden())
            names << item->text();
    }
    return names;
}

void ModelListWidget::onItemClicked(QListWidgetItem *item)
{
    if (item)
        emit modelSelected(item->text());
}

void ModelListWidget::onItemDoubleClicked(QListWidgetItem *item)
{
    if (item)
        emit modelDoubleClicked(item->text());
}

void ModelListWidget::onContextMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_list->itemAt(pos);
    if (!item)
        return;

    const QString modelName = item->text();

    QMenu menu(this);
    QAction *editAction = menu.addAction(T("ctx.edit"));
    QAction *renameAction = menu.addAction(T("ctx.rename"));
    menu.addSeparator();
    QAction *deleteAction = menu.addAction(T("ctx.delete"));
    deleteAction->setIcon(QApplication::style()->standardIcon(QStyle::SP_TrashIcon));

    QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(pos));
    if (chosen == editAction) {
        emit modelSelected(modelName);
    } else if (chosen == renameAction) {
        emit renameRequested(modelName);
    } else if (chosen == deleteAction) {
        emit deleteRequested(modelName);
    }
}

void ModelListWidget::onFilterChanged(const QString &text)
{
    for (int i = 0; i < m_list->count(); ++i) {
        QListWidgetItem *item = m_list->item(i);
        item->setHidden(!item->text().contains(text, Qt::CaseInsensitive));
    }
}

void ModelListWidget::onDeleteClicked()
{
    QString name = currentModel();
    if (!name.isEmpty()) {
        emit deleteRequested(name);
    }
}
