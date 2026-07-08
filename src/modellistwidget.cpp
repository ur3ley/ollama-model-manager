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

    m_tree = new QTreeWidget();
    m_tree->setColumnWidth(0, 220);
    m_tree->setColumnWidth(1, 70);
    m_tree->setRootIsDecorated(false);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAlternatingRowColors(true);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->header()->setStretchLastSection(false);
    layout->addWidget(m_tree, 1);

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
    connect(m_tree, &QTreeWidget::itemClicked, this, &ModelListWidget::onItemClicked);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, &ModelListWidget::onItemDoubleClicked);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &ModelListWidget::onContextMenu);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this]() {
        m_deleteBtn->setEnabled(m_tree->currentItem() != nullptr);
    });
    connect(m_filterEdit, &QLineEdit::textChanged, this, &ModelListWidget::onFilterChanged);

    retranslateUi();
    connect(I18n::instance(), &I18n::languageChanged, this, &ModelListWidget::retranslateUi);
}

void ModelListWidget::retranslateUi()
{
    m_title->setText(T("list.title"));
    m_refreshBtn->setText(T("list.refresh"));
    m_refreshBtn->setToolTip(T("list.refreshTip"));
    m_filterEdit->setPlaceholderText(T("list.filter"));
    m_tree->setHeaderLabels({T("list.colName"), T("list.colSize")});
    m_tree->setColumnWidth(0, 220);
    m_tree->setColumnWidth(1, 70);
    m_newBtn->setText(T("list.new"));
    m_newBtn->setToolTip(T("list.newTip"));
    m_importBtn->setText(T("list.import"));
    m_importBtn->setToolTip(T("list.importTip"));
    m_deleteBtn->setText(T("list.delete"));
    m_deleteBtn->setToolTip(T("list.deleteTip"));
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
        item->setToolTip(0, T("list.itemTip").arg(model.id, model.modified));
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
    QAction *editAction = menu.addAction(T("ctx.edit"));
    QAction *renameAction = menu.addAction(T("ctx.rename"));
    menu.addSeparator();
    QAction *deleteAction = menu.addAction(T("ctx.delete"));
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
