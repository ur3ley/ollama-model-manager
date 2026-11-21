#include "modelbrowserpanel.h"

#include "carddelegate.h"
#include "designtokens.h"
#include "i18n.h"

#include <QComboBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QTimer>
#include <QVBoxLayout>

namespace {

} // namespace

ModelBrowserPanel::ModelBrowserPanel(const QString &placeholder, QWidget *parent)
    : QWidget(parent)
    , m_actionText(T("browser.download"))
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(16);

    auto *left = new QWidget();
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    // ------------------------------------------------------------- card list
    m_list = new QListWidget();
    m_list->setItemDelegate(new CardDelegate(m_list));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setMouseTracking(true);
    m_list->setUniformItemSizes(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFixedWidth(380);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; }"
        "QListWidget::item { background: transparent; }"));
    leftLayout->addWidget(m_list, 1);

    m_emptyLabel = new QLabel(placeholder);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setStyleSheet(QStringLiteral("color:%1;").arg(tokens::Muted));
    leftLayout->addWidget(m_emptyLabel);
    left->setFixedWidth(380);
    root->addWidget(left);

    // ---------------------------------------------------------- detail pane
    auto *detailScroll = new QScrollArea();
    detailScroll->setWidgetResizable(true);
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *detail = new QWidget();
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(4, 4, 4, 4);
    detailLayout->setSpacing(10);

    m_titleLabel = new QLabel();
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 7);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setWordWrap(true);
    detailLayout->addWidget(m_titleLabel);

    m_subtitleLabel = new QLabel();
    m_subtitleLabel->setStyleSheet(QStringLiteral("color:%1;").arg(tokens::Muted));
    detailLayout->addWidget(m_subtitleLabel);

    m_badgesRow = new QWidget();
    auto *badgesLayout = new QHBoxLayout(m_badgesRow);
    badgesLayout->setContentsMargins(0, 0, 0, 0);
    badgesLayout->setSpacing(6);
    badgesLayout->addStretch();
    detailLayout->addWidget(m_badgesRow);

    m_statsLabel = new QLabel();
    m_statsLabel->setStyleSheet(QStringLiteral("color:%1;").arg(tokens::Muted));
    detailLayout->addWidget(m_statsLabel);

    // variant picker row
    auto *variantRow = new QHBoxLayout();
    variantRow->setSpacing(8);
    m_variantCombo = new QComboBox();
    m_variantCombo->setMinimumWidth(240);
    m_variantCombo->setStyleSheet(tokens::comboStyle());
    variantRow->addWidget(m_variantCombo, 1);
    m_variantSize = new QLabel();
    m_variantSize->setStyleSheet(QStringLiteral("color:%1;").arg(tokens::Muted));
    variantRow->addWidget(m_variantSize);
    variantRow->addStretch();
    m_actionBtn = new QPushButton(m_actionText);
    m_actionBtn->setObjectName("primary");
    m_actionBtn->setMinimumWidth(130);
    m_actionBtn->setStyleSheet(tokens::primaryButtonStyle());
    variantRow->addWidget(m_actionBtn);
    m_cancelBtn = new QPushButton(T("browser.cancel"));
    m_cancelBtn->setVisible(false);
    m_cancelBtn->setStyleSheet(tokens::ghostButtonStyle());
    variantRow->addWidget(m_cancelBtn);
    detailLayout->addLayout(variantRow);

    m_progress = new QProgressBar();
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(true);
    m_progress->setVisible(false);
    m_progress->setStyleSheet(tokens::progressStyle());
    detailLayout->addWidget(m_progress);

    m_statusLabel = new QLabel();
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color:%1; font-size:11px;").arg(tokens::Muted));
    detailLayout->addWidget(m_statusLabel);

    m_descriptionLabel = new QLabel();
    m_descriptionLabel->setWordWrap(true);
    m_descriptionLabel->setTextFormat(Qt::PlainText);
    m_descriptionLabel->setStyleSheet(QStringLiteral("font-size:13px; line-height:1.4;"));
    detailLayout->addWidget(m_descriptionLabel);
    detailLayout->addStretch();

    detailScroll->setWidget(detail);
    root->addWidget(detailScroll, 1);

    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() {
        auto *item = m_list->currentItem();
        m_selectedId = item ? item->data(cardrole::Id).toString() : QString();
        updateDetail();
        if (!m_selectedId.isEmpty())
            emit itemSelected(m_selectedId);
    });
    connect(m_variantCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateActionState(); });
    connect(m_actionBtn, &QPushButton::clicked, this, [this]() {
        const QString variant = selectedVariantId();
        if (!m_selectedId.isEmpty() && !variant.isEmpty())
            emit actionRequested(m_selectedId, variant);
    });
    connect(m_cancelBtn, &QPushButton::clicked, this, &ModelBrowserPanel::cancelRequested);

    updateDetail();
}

void ModelBrowserPanel::selectVariant(int index)
{
    if (index >= 0 && index < m_variantCombo->count())
        m_variantCombo->setCurrentIndex(index);
}

void ModelBrowserPanel::selectVariantById(const QString &variantId)
{
    for (int i = 0; i < m_variantCombo->count(); ++i) {
        if (m_variantCombo->itemData(i, Qt::UserRole + 1).toString() == variantId) {
            m_variantCombo->setCurrentIndex(i);
            return;
        }
    }
}

void ModelBrowserPanel::setEmptyText(const QString &text)
{
    m_emptyLabel->setText(text);
}

void ModelBrowserPanel::setActionText(const QString &text)
{
    m_actionText = text;
    m_actionBtn->setText(text);
}

void ModelBrowserPanel::setItems(const QList<BrowserItem> &items)
{
    // Large sources (the whole Ollama library is 270+ models) are inserted in
    // small batches so the window stays responsive while the list fills up.
    m_items = items;
    m_selectedId.clear();
    m_pendingItems = items;
    m_pendingIndex = 0;

    m_list->clear();
    m_emptyLabel->setVisible(items.isEmpty());
    m_list->setVisible(!items.isEmpty());

    if (items.isEmpty()) {
        updateDetail();
        updateActionState();
        return;
    }

    if (!m_batchTimer) {
        m_batchTimer = new QTimer(this);
        m_batchTimer->setInterval(0);
        connect(m_batchTimer, &QTimer::timeout, this, [this]() {
            appendBatch();
        });
    }
    m_batchTimer->start();
}

void ModelBrowserPanel::appendBatch()
{
    constexpr int kBatch = 24;
    m_list->blockSignals(true);
    for (int i = 0; i < kBatch && m_pendingIndex < m_pendingItems.size(); ++i, ++m_pendingIndex) {
        const BrowserItem &item = m_pendingItems.at(m_pendingIndex);
        auto *row = new QListWidgetItem(m_list);
        row->setData(cardrole::Id, item.id);
        row->setData(cardrole::Title, item.title);
        row->setData(cardrole::Subtitle, item.subtitle);
        row->setData(cardrole::Stats, item.stats);
    }
    const bool first = m_list->count() > 0 && !m_list->currentItem();
    const bool done = m_pendingIndex >= m_pendingItems.size();
    m_list->blockSignals(false);

    if (first)
        m_list->setCurrentRow(0);
    if (done) {
        m_batchTimer->stop();
        m_pendingItems.clear();
        updateActionState();
    }
}

void ModelBrowserPanel::clearItems()
{
    setItems({});
}

void ModelBrowserPanel::selectItem(int row)
{
    if (row >= 0 && row < m_list->count())
        m_list->setCurrentRow(row);
}

int ModelBrowserPanel::itemCount() const
{
    return m_list->count();
}

int ModelBrowserPanel::variantCount() const
{
    return m_variantCombo->count();
}

QString ModelBrowserPanel::selectedItemId() const
{
    return m_selectedId;
}

QString ModelBrowserPanel::selectedVariantId() const
{
    const int index = m_variantCombo->currentIndex();
    if (index < 0)
        return QString();
    return m_variantCombo->itemData(index, Qt::UserRole + 1).toString();
}

const BrowserItem *ModelBrowserPanel::itemById(const QString &id) const
{
    for (const BrowserItem &item : m_items) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

BrowserItem *ModelBrowserPanel::itemById(const QString &id)
{
    for (BrowserItem &item : m_items) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

void ModelBrowserPanel::setVariants(const QString &itemId, const QList<BrowserVariant> &variants)
{
    BrowserItem *item = itemById(itemId);
    if (!item)
        return;
    item->variants = variants;
    item->variantsLoaded = true;
    if (itemId == m_selectedId)
        updateDetail();
}

void ModelBrowserPanel::setVariantsError(const QString &itemId, const QString &error)
{
    BrowserItem *item = itemById(itemId);
    if (!item)
        return;
    item->variantsLoaded = true;
    item->variants.clear();
    if (itemId == m_selectedId) {
        updateDetail();
        m_statusLabel->setText(error);
    }
}

void ModelBrowserPanel::patchVariant(const QString &itemId, const QString &variantId,
                                     const QString &suffix)
{
    BrowserItem *item = itemById(itemId);
    if (!item)
        return;
    bool changed = false;
    for (BrowserVariant &variant : item->variants) {
        if (variant.id == variantId) {
            variant.label += suffix;
            changed = true;
            break;
        }
    }
    if (!changed || itemId != m_selectedId)
        return;

    // Update the combo in place: a full updateDetail() would rebuild the list
    // and drop the user's current selection.
    for (int row = 0; row < m_variantCombo->count(); ++row) {
        if (m_variantCombo->itemData(row, Qt::UserRole + 1).toString() != variantId)
            continue;
        for (const BrowserVariant &variant : item->variants) {
            if (variant.id != variantId)
                continue;
            QString label = variant.label;
            if (!variant.size.isEmpty())
                label += QStringLiteral("  ·  ") + variant.size;
            m_variantCombo->setItemText(row, label);
            break;
        }
        break;
    }
}

void ModelBrowserPanel::updateDetail()
{
    const BrowserItem *item = itemById(m_selectedId);
    const bool hasItem = item != nullptr;

    m_titleLabel->setVisible(hasItem);
    m_subtitleLabel->setVisible(hasItem);
    m_badgesRow->setVisible(hasItem);
    m_statsLabel->setVisible(hasItem);
    m_descriptionLabel->setVisible(hasItem);
    m_variantCombo->setEnabled(hasItem && !m_busy);
    m_variantSize->setVisible(hasItem);

    if (!hasItem) {
        m_titleLabel->setText(T("browser.noSelection"));
        m_titleLabel->setVisible(true);
        m_subtitleLabel->clear();
        m_descriptionLabel->clear();
        m_statsLabel->clear();
        m_variantCombo->clear();
        m_variantSize->clear();
        updateActionState();
        return;
    }

    m_titleLabel->setText(item->title);
    m_subtitleLabel->setText(item->subtitle);
    m_statsLabel->setText(item->stats);
    m_descriptionLabel->setText(item->description);

    // Rebuild the badge chips.
    auto *badgesLayout = qobject_cast<QHBoxLayout *>(m_badgesRow->layout());
    while (QLayoutItem *child = badgesLayout->takeAt(0)) {
        if (child->widget())
            child->widget()->deleteLater();
        delete child;
    }
    for (const QString &badge : item->badges) {
        auto *chip = new QLabel(badge);
        chip->setStyleSheet(QStringLiteral("%1").arg(tokens::chipStyle(badge)));
        badgesLayout->addWidget(chip);
    }
    badgesLayout->addStretch();

    m_variantCombo->blockSignals(true);
    m_variantCombo->clear();
    for (const BrowserVariant &variant : item->variants) {
        QString label = variant.label;
        if (!variant.size.isEmpty())
            label += QStringLiteral("  ·  ") + variant.size;
        if (!variant.enabled)
            label += QStringLiteral("  ·  ") + (variant.hint.isEmpty() ? T("browser.disabled")
                                                                       : variant.hint);
        m_variantCombo->addItem(label);
        const int row = m_variantCombo->count() - 1;
        m_variantCombo->setItemData(row, variant.id, Qt::UserRole + 1);
        if (!variant.enabled) {
            // Grey out unsupported entries but keep them visible as an explanation.
            m_variantCombo->setItemData(row, QColor(tokens::Muted), Qt::ForegroundRole);
            if (auto *model = qobject_cast<QStandardItemModel *>(m_variantCombo->model()))
                model->item(row)->setEnabled(false);
        }
    }
    m_variantCombo->setEnabled(!m_busy && !item->variants.isEmpty());
    m_variantCombo->blockSignals(false);

    if (!item->variantsLoaded) {
        m_variantSize->setText(T("browser.loadingVariants"));
        m_statusLabel->setText(QString());
    } else if (item->variants.isEmpty()) {
        m_variantSize->setText(QString());
        m_statusLabel->setText(T("browser.noVariants"));
    } else {
        m_statusLabel->setText(QString());
    }
    updateActionState();
}

void ModelBrowserPanel::updateActionState()
{
    const BrowserItem *item = itemById(m_selectedId);
    bool usable = false;
    if (item && item->variantsLoaded) {
        const int index = m_variantCombo->currentIndex();
        if (index >= 0 && index < item->variants.size())
            usable = item->variants.at(index).enabled;

        if (index >= 0 && index < item->variants.size()) {
            const BrowserVariant &variant = item->variants.at(index);
            m_variantSize->setText(variant.hint);
        }
    }
    m_actionBtn->setEnabled(!m_busy && usable);
    m_actionBtn->setVisible(!m_busy);
    m_cancelBtn->setVisible(m_busy);
}

void ModelBrowserPanel::setBusy(bool busy, int percent)
{
    m_busy = busy;
    m_progress->setVisible(busy);
    if (busy)
        m_progress->setValue(qBound(0, percent, 100));
    m_list->setEnabled(!busy);
    m_variantCombo->setEnabled(!busy && itemById(m_selectedId) != nullptr);
    updateActionState();
}

void ModelBrowserPanel::setStatus(const QString &status)
{
    m_statusLabel->setText(status);
}
