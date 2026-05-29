#include "modellistwidget.h"
#include <QHeaderView>
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
}

void ModelListWidget::setupUI()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);

    auto *headerLayout = new QHBoxLayout();
    auto *title = new QLabel(tr("Модели"));
    QFont boldFont = title->font();
    boldFont.setBold(true);
    boldFont.setPointSize(boldFont.pointSize() + 2);
    title->setFont(boldFont);
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    m_refreshBtn = new QPushButton(tr("Обновить"));
    m_refreshBtn->setToolTip(tr("Обновить список моделей"));
    headerLayout->addWidget(m_refreshBtn);
    layout->addLayout(headerLayout);

    m_filterEdit = new QLineEdit();
    m_filterEdit->setPlaceholderText(tr("Фильтр..."));
    m_filterEdit->setClearButtonEnabled(true);
    layout->addWidget(m_filterEdit);

    m_tree = new QTreeWidget();
    m_tree->setHeaderLabels({tr("Имя"), tr("Размер")});
    m_tree->setColumnWidth(0, 220);
    m_tree->setColumnWidth(1, 70);
    m_tree->setRootIsDecorated(false);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAlternatingRowColors(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(false);
    layout->addWidget(m_tree, 1);

    auto *btnLayout = new QHBoxLayout();
    m_newBtn = new QPushButton(tr("Новая модель"));
    m_newBtn->setToolTip(tr("Создать новый вариант модели"));
    btnLayout->addWidget(m_newBtn);
    auto *importBtn = new QPushButton(tr("Импорт GGUF"));
    importBtn->setToolTip(tr("Импортировать модель из GGUF-файла"));
    btnLayout->addWidget(importBtn);
    m_deleteBtn = new QPushButton(tr("Удалить"));
    m_deleteBtn->setToolTip(tr("Удалить выбранную модель"));
    m_deleteBtn->setEnabled(false);
    btnLayout->addWidget(m_deleteBtn);
    layout->addLayout(btnLayout);

    connect(m_refreshBtn, &QPushButton::clicked, this, &ModelListWidget::refresh);
    connect(m_newBtn, &QPushButton::clicked, this, &ModelListWidget::newModelRequested);
    connect(importBtn, &QPushButton::clicked, this, &ModelListWidget::importGGUFRequested);
    connect(m_deleteBtn, &QPushButton::clicked, this, &ModelListWidget::onDeleteClicked);
    connect(m_tree, &QTreeWidget::itemClicked, this, &ModelListWidget::onItemClicked);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &ModelListWidget::onItemDoubleClicked);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &ModelListWidget::onContextMenu);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        m_deleteBtn->setEnabled(m_tree->currentItem() != nullptr);
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, &ModelListWidget::onFilterChanged);
}

void ModelListWidget::refresh()
{
    QList<OllamaCli::ModelInfo> models = m_cli->listModels();
    populateList(models);
}

void ModelListWidget::populateList(const QList<OllamaCli::ModelInfo> &models)
{
    m_tree->clear();

    for (const auto &model : models) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(0, model.name);
        item->setText(1, model.size);
        item->setToolTip(0, tr("ID: %1\nИзменён: %2").arg(model.id, model.modified));
        item->setData(0, Qt::UserRole, model.id);
        item->setData(0, Qt::UserRole + 1, model.modified);
    }
}

QString ModelListWidget::currentModel() const
{
    QTreeWidgetItem *item = m_tree->currentItem();
    if (item) {
        return item->text(0);
    }
    return QString();
}

QStringList ModelListWidget::modelNames() const
{
    QStringList names;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_tree->topLevelItem(i);
        if (!item->isHidden()) {
            names << item->text(0);
        }
    }
    return names;
}

void ModelListWidget::onItemClicked(QTreeWidgetItem *item, int)
{
    if (item) {
        emit modelSelected(item->text(0));
    }
}

void ModelListWidget::onItemDoubleClicked(QTreeWidgetItem *item, int)
{
    if (item) {
        emit modelDoubleClicked(item->text(0));
    }
}

void ModelListWidget::onContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item) return;

    QString modelName = item->text(0);

    QMenu menu(this);
    QAction *editAction = menu.addAction(tr("Редактировать"));
    QAction *renameAction = menu.addAction(tr("Переименовать"));
    menu.addSeparator();
    QAction *deleteAction = menu.addAction(tr("Удалить"));
    deleteAction->setIcon(QApplication::style()->standardIcon(QStyle::SP_TrashIcon));

    QAction *chosen = menu.exec(m_tree->viewport()->mapToGlobal(pos));
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
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_tree->topLevelItem(i);
        bool matches = item->text(0).contains(text, Qt::CaseInsensitive);
        item->setHidden(!matches);
    }
}

void ModelListWidget::onDeleteClicked()
{
    QString name = currentModel();
    if (!name.isEmpty()) {
        emit deleteRequested(name);
    }
}
