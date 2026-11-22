#include "modelbrowserpanel.h"

#include "combohighlightdelegate.h"

#include "carddelegate.h"
#include "doticons.h"
#include "memoryinfo.h"
#include "designtokens.h"
#include "i18n.h"
#include "quantmatch.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QStandardItemModel>
#include <QStyle>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace {

// Variant picker with a bounded popup: a big GGUF repo publishes dozens of
// quants and the raw file list used to produce hundreds of rows. The popup is
// capped in height, elides long labels in the middle and never grows past the
// combo width or the screen it opens on.
class VariantComboBox : public QComboBox
{
    Q_OBJECT
public:
    explicit VariantComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
        setMaxVisibleItems(kMaxVisibleRows);
        view()->setTextElideMode(Qt::ElideMiddle);
        view()->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    void showPopup() override
    {
        // Never narrower than the closed combo. Size the list before the
        // container lays itself out, so the popup is an exact number of rows
        // plus the view's own 1px border and no row is clipped in half.
        view()->setMinimumWidth(width());
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
        const int maxHeight = wantedView + chrome;
        if (popup->height() > maxHeight)
            popup->resize(popup->width(), maxHeight);

        const QRect available = screen() ? screen()->availableGeometry() : QRect();
        int popupWidth = width();
        if (available.isValid())
            popupWidth = qMin(popupWidth, available.width() - 16);
        popupWidth = qMax(popupWidth, 240);
        popup->resize(popupWidth, popup->height());
        if (!available.isValid())
            return;
        // Qt decides above/below using the full (unclamped) list height, so a
        // long list can flip the popup over the search field even though the
        // bounded list would fit below. Place it again from the combo rect.
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

// Mixture-of-experts architectures rename the family ("qwen3moe"), while a few
// keep the plain name; anything that is not a known MoE architecture is dense.
bool isMoeArchitecture(const QString &architecture)
{
    if (architecture.isEmpty())
        return false;
    const QString arch = architecture.toLower();
    if (arch.contains(QLatin1String("moe")))
        return true;
    static const QStringList known = {
        QStringLiteral("mixtral"), QStringLiteral("deepseek2"),
        QStringLiteral("deepseek3"), QStringLiteral("grok")
    };
    return known.contains(arch);
}

// Max characters of a license name in the one-line characteristics block:
// longer ones ("Apache License 2.0 with LLVM exceptions") would wrap the row.
constexpr int kLicenseChars = 34;

QString elideLicense(const QString &text)
{
    if (text.size() <= kLicenseChars)
        return text;
    return text.left(kLicenseChars - 1).trimmed() + QStringLiteral("…");
}

// "1 year ago" / "2 months ago" comes from the registry page in English. The
// wording itself lives in the language files, so the Russian UI says
// "2 месяца назад" instead of a half-translated phrase.
QString localizedAgo(const QString &raw)
{
    static const QRegularExpression re(QStringLiteral("^(\\d+)\\s+(\\w+)\\s+ago$"));
    const QRegularExpressionMatch match = re.match(raw.trimmed());
    if (!match.hasMatch())
        return raw;

    const qint64 count = match.captured(1).toLongLong();
    if (count <= 0)
        return T("browser.updatedAgoNow");

    const QString unit = match.captured(2).toLower();
    QString key;
    if (unit.startsWith(QLatin1String("year")))
        key = QStringLiteral("browser.updatedAgoYear");
    else if (unit.startsWith(QLatin1String("month")))
        key = QStringLiteral("browser.updatedAgoMonth");
    else if (unit.startsWith(QLatin1String("week")))
        key = QStringLiteral("browser.updatedAgoWeek");
    else if (unit.startsWith(QLatin1String("day")))
        key = QStringLiteral("browser.updatedAgoDay");
    else if (unit.startsWith(QLatin1String("hour")))
        key = QStringLiteral("browser.updatedAgoHour");
    else if (unit.startsWith(QLatin1String("minute")))
        key = QStringLiteral("browser.updatedAgoMinute");
    else if (unit.startsWith(QLatin1String("second")))
        return T("browser.updatedAgoNow");
    else
        return raw;

    // Slavic plural categories. English spells "few" and "many" the same way,
    // so one rule serves both files.
    const qint64 mod100 = count % 100;
    const qint64 mod10 = count % 10;
    QString plural = QStringLiteral("Many");
    if (mod10 == 1 && mod100 != 11)
        plural = QStringLiteral("One");
    else if (mod10 >= 2 && mod10 <= 4 && (mod100 < 12 || mod100 > 14))
        plural = QStringLiteral("Few");

    const QString text = T(key + plural);
    if (text == key + plural)
        return raw; // not translated in this language file
    return text.arg(count);
}

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
    m_list->setObjectName(QStringLiteral("browserList"));
    m_list->setItemDelegate(new CardDelegate(m_list));
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setMouseTracking(true);
    m_list->setUniformItemSizes(true);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFixedWidth(380);
    m_list->setStyleSheet(QStringLiteral(
        "QListWidget { background: transparent; border: none; }"
        "QListWidget::item { background: transparent; padding: 0px;"
        " border-radius: 0px; }"));
    leftLayout->addWidget(m_list, 1);

    m_emptyLabel = new QLabel(placeholder);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    tokens::setMuted(m_emptyLabel);
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
    tokens::setMuted(m_subtitleLabel);
    detailLayout->addWidget(m_subtitleLabel);

    m_badgesRow = new QWidget();
    auto *badgesLayout = new QHBoxLayout(m_badgesRow);
    badgesLayout->setContentsMargins(0, 0, 0, 0);
    badgesLayout->setSpacing(6);
    badgesLayout->addStretch();
    detailLayout->addWidget(m_badgesRow);

    m_statsLabel = new QLabel();
    tokens::setMuted(m_statsLabel);
    detailLayout->addWidget(m_statsLabel);

    // -------------------------------------------------- model characteristics
    // One wrapped line: "MoE · параметры 30.5B · квант Q4_K_M · контекст 40K".
    m_specsLabel = new QLabel();
    m_specsLabel->setTextFormat(Qt::RichText);
    m_specsLabel->setWordWrap(true);
    m_specsLabel->setVisible(false);
    detailLayout->addWidget(m_specsLabel);

    m_descriptionLabel = new QLabel();
    m_descriptionLabel->setWordWrap(true);
    m_descriptionLabel->setTextFormat(Qt::PlainText);
    detailLayout->addWidget(m_descriptionLabel);

    // --------------------------------------------------------- about/readme
    // Collapsed by default: the readme is long and most of the time the user
    // is only picking a quantization.
    m_aboutToggle = new QToolButton();
    m_aboutToggle->setObjectName(QStringLiteral("aboutToggle"));
    m_aboutToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_aboutToggle->setArrowType(Qt::RightArrow);
    m_aboutToggle->setText(T("browser.about"));
    m_aboutToggle->setCheckable(true);
    m_aboutToggle->setVisible(false);
    detailLayout->addWidget(m_aboutToggle, 0, Qt::AlignLeft);

    m_about = new QTextBrowser();
    m_about->setOpenExternalLinks(true);
    m_about->setVisible(false);
    m_about->setMinimumWidth(240);
    m_about->setFixedHeight(260);
    detailLayout->addWidget(m_about);

    // variant picker row
    auto *variantRow = new QHBoxLayout();
    variantRow->setSpacing(8);
    m_variantCombo = new VariantComboBox();
    m_variantCombo->setMinimumWidth(240);
    m_variantCombo->view()->setItemDelegate(
            new ComboHighlightDelegate(m_variantCombo->view()));
    variantRow->addWidget(m_variantCombo, 1);
    m_variantSize = new QLabel();
    tokens::setMuted(m_variantSize);
    variantRow->addWidget(m_variantSize);
    variantRow->addStretch();
    m_actionBtn = new QPushButton(m_actionText);
    m_actionBtn->setObjectName("primary");
    m_actionBtn->setProperty("accent", true);
    m_actionBtn->setMinimumWidth(130);
    variantRow->addWidget(m_actionBtn);
    m_cancelBtn = new QPushButton(T("browser.cancel"));
    m_cancelBtn->setVisible(false);
    variantRow->addWidget(m_cancelBtn);
    detailLayout->addLayout(variantRow);

    m_progress = new QProgressBar();
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(true);
    m_progress->setVisible(false);
    detailLayout->addWidget(m_progress);

    m_statusLabel = new QLabel();
    m_statusLabel->setObjectName(QStringLiteral("browserStatus"));
    m_statusLabel->setWordWrap(true);
    tokens::setMuted(m_statusLabel);
    tokens::setCaption(m_statusLabel);
    detailLayout->addWidget(m_statusLabel);
    detailLayout->addStretch();

    detailScroll->setWidget(detail);
    root->addWidget(detailScroll, 1);

    connect(m_list, &QListWidget::itemSelectionChanged, this, [this]() {
        auto *item = m_list->currentItem();
        m_selectedId = item ? item->data(cardrole::Id).toString() : QString();
        // A different model means a different readme and different per-quant
        // facts: never carry the previous one over.
        m_requestedVariant.clear();
        updateDetail();
        if (!m_selectedId.isEmpty())
            emit itemSelected(m_selectedId);
    });
    connect(m_variantCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
        updateActionState();
        updateSpecs();
        requestCurrentVariant();
    });
    connect(m_aboutToggle, &QToolButton::toggled, this, [this](bool on) {
        m_aboutToggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        m_about->setVisible(on && !m_renderedMarkdown.isEmpty());
    });
    connect(m_actionBtn, &QPushButton::clicked, this, [this]() {
        const QString variant = selectedVariantId();
        if (!m_selectedId.isEmpty() && !variant.isEmpty())
            emit actionRequested(m_selectedId, variant);
    });
    connect(m_cancelBtn, &QPushButton::clicked, this, &ModelBrowserPanel::cancelRequested);
    connect(theme::Theme::instance(), &theme::Theme::changed, this, [this]() {
        for (QLabel *chip : std::as_const(m_chips))
            chip->setStyleSheet(tokens::chipStyle(chip->text()));
        applyVariantColors();
        // Markdown is cheap to re-parse and this keeps the readme readable
        // after a switch to the other palette.
        m_renderedMarkdown.clear();
        applyAboutStyle();
        updateSpecs();
        updateAbout();
        m_list->viewport()->update();
    });

    applyAboutStyle();
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
    // The visible text depends on the state of the selected variant, so let
    // updateActionState() decide instead of overwriting it here.
    updateActionState();
}

void ModelBrowserPanel::setItems(const QList<BrowserItem> &items)
{
    // Large sources (the whole Ollama library is 270+ models) are inserted in
    // small batches so the window stays responsive while the list fills up.
    m_items = items;
    m_selectedId.clear();
    m_requestedVariant.clear();
    m_renderedMarkdown.clear();
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

QString ModelBrowserPanel::selectedVariantSizeText() const
{
    const QString variant = selectedVariantId();
    if (variant.isEmpty())
        return QString();
    const BrowserItem *item = itemById(m_selectedId);
    if (!item)
        return QString();
    for (const BrowserVariant &candidate : item->variants) {
        if (candidate.id == variant)
            return candidate.size;
    }
    return QString();
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

void ModelBrowserPanel::setVariantState(const QString &itemId, const QString &variantId,
                                        bool installed, bool updateAvailable)
{
    BrowserItem *item = itemById(itemId);
    if (!item || variantId.isEmpty())
        return;

    BrowserVariant *changed = nullptr;
    for (BrowserVariant &variant : item->variants) {
        if (variant.id != variantId)
            continue;
        if (variant.installed == installed
                && variant.updateAvailable == (installed && updateAvailable))
            return; // nothing new: do not touch the combo
        variant.installed = installed;
        // "Update" only makes sense on top of a local copy.
        variant.updateAvailable = installed && updateAvailable;
        changed = &variant;
        break;
    }
    if (!changed || itemId != m_selectedId)
        return;

    // Rewrite one row instead of rebuilding the picker: an update check lands
    // while the user may be scrolling or about to press the button.
    for (int row = 0; row < m_variantCombo->count(); ++row) {
        if (m_variantCombo->itemData(row, Qt::UserRole + 1).toString() != variantId)
            continue;
        updateVariantRow(row, *changed);
        break;
    }
    if (selectedVariantId() == variantId)
        updateActionState();
}

void ModelBrowserPanel::setModelDetails(const QString &itemId, const BrowserAbout &about)
{
    BrowserItem *item = itemById(itemId);
    if (!item)
        return;
    item->about = about;
    item->aboutLoaded = true;
    if (itemId == m_selectedId)
        updateDetail();
}

void ModelBrowserPanel::setTagDetails(const QString &itemId, const QString &variantId,
                                      const BrowserSpecs &specs)
{
    BrowserItem *item = itemById(itemId);
    if (!item || variantId.isEmpty())
        return;
    item->specs.insert(variantId, specs);
    // Only repaint when the data belongs to the visible pair, so a late reply
    // for a previous selection cannot leak into the current one.
    if (itemId == m_selectedId && variantId == selectedVariantId())
        updateSpecs();
}

void ModelBrowserPanel::setAboutExpanded(bool expanded)
{
    m_aboutToggle->setChecked(expanded);
}

bool ModelBrowserPanel::isAboutExpanded() const
{
    return m_aboutToggle->isChecked();
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
        for (const BrowserVariant &variant : std::as_const(item->variants)) {
            if (variant.id != variantId)
                continue;
            updateVariantRow(row, variant);
            break;
        }
        break;
    }
}

QString ModelBrowserPanel::variantLabel(const BrowserVariant &variant) const
{
    QString label = variant.label;
    if (!variant.size.isEmpty())
        label += QStringLiteral("  ·  ") + variant.size;
    if (!variant.enabled) {
        label += QStringLiteral("  ·  ")
                + (variant.hint.isEmpty() ? T("browser.disabled") : variant.hint);
        return label;
    }

    // Install state comes right after the size: it is what the action button
    // on the same row does.
    if (variant.updateAvailable)
        label += QStringLiteral("  ·  ") + T("browser.updateMark");
    else if (variant.installed)
        label += QStringLiteral("  ·  ") + T("browser.installedMark");

    if (variant.fit == memoryinfo::Fit::No)
        label += QStringLiteral("  ·  ") + T("browser.noFit");
    else if (variant.fit == memoryinfo::Fit::Tight)
        label += QStringLiteral("  ·  ") + T("browser.mayNotFit");
    return label;
}

void ModelBrowserPanel::updateVariantRow(int row, const BrowserVariant &variant)
{
    if (row < 0 || row >= m_variantCombo->count())
        return;
    m_variantCombo->setItemText(row, variantLabel(variant));
    m_variantCombo->setItemIcon(
            row, dotIcon(variant.enabled ? variant.fit : memoryinfo::Fit::Unknown));
}

void ModelBrowserPanel::updateSpecs()
{
    const BrowserItem *item = itemById(m_selectedId);
    const QString variantId = selectedVariantId();
    if (!item || variantId.isEmpty()) {
        m_specsLabel->clear();
        m_specsLabel->setVisible(false);
        return;
    }

    const BrowserSpecs specs = item->specs.value(variantId);
    const BrowserVariant *variant = nullptr;
    for (const BrowserVariant &candidate : item->variants) {
        if (candidate.id == variantId) {
            variant = &candidate;
            break;
        }
    }

    m_specsLabel->setToolTip(QString());

    const theme::Palette &p = tokens::activePalette();
    auto key = [&p](const QString &text) {
        return QStringLiteral("<span style=\"color:%1\">%2</span>")
                .arg(p.muted.name(), text);
    };
    auto value = [](const QString &text) {
        return QStringLiteral("<b>") + text.toHtmlEscaped() + QStringLiteral("</b>");
    };
    auto pair = [&key, &value](const QString &label, const QString &text) {
        return key(label) + QLatin1Char(' ') + value(text);
    };

    QStringList parts;
    if (!specs.architecture.isEmpty()) {
        parts << value(isMoeArchitecture(specs.architecture) ? T("browser.moe")
                                                             : T("browser.dense"));
    }
    if (!specs.parameters.isEmpty())
        parts << pair(T("browser.specsParams"), specs.parameters);
    if (!specs.quantization.isEmpty())
        parts << pair(T("browser.specsQuant"), specs.quantization);
    if (variant && !variant->context.isEmpty())
        parts << pair(T("browser.specsContext"), variant->context);
    if (variant && !variant->input.isEmpty())
        parts << pair(T("browser.specsInput"), variant->input);
    if (!specs.license.isEmpty()) {
        parts << pair(T("browser.specsLicense"), elideLicense(specs.license));
        if (specs.license.size() > kLicenseChars)
            m_specsLabel->setToolTip(specs.license);
    }

    if (parts.isEmpty()) {
        m_specsLabel->clear();
        m_specsLabel->setVisible(false);
        return;
    }
    m_specsLabel->setText(parts.join(QStringLiteral("  ·  ")));
    m_specsLabel->setVisible(true);
}

void ModelBrowserPanel::updateAbout()
{
    const BrowserItem *item = itemById(m_selectedId);
    const QString markdown = item ? item->about.readmeMarkdown : QString();

    if (markdown.isEmpty()) {
        m_aboutToggle->setVisible(false);
        m_aboutToggle->setChecked(false);
        m_about->setVisible(false);
        m_about->clear();
        m_renderedMarkdown.clear();
        return;
    }

    m_aboutToggle->setVisible(true);
    if (markdown == m_renderedMarkdown)
        return;

    // Markdown images do not survive the trip: the readme links to model logos
    // and screenshots on the library host, which would be fetched one by one.
    static const QRegularExpression mdImage(QStringLiteral("!\\[[^\\]]*\\]\\([^)]*\\)"));
    static const QRegularExpression htmlImage(QStringLiteral("<img[^>]*>"),
                                             QRegularExpression::CaseInsensitiveOption);
    QString text = markdown;
    text.remove(mdImage);
    text.remove(htmlImage);

    m_renderedMarkdown = markdown;
    m_about->setMarkdown(text);
    m_about->moveCursor(QTextCursor::Start);
    m_about->setVisible(m_aboutToggle->isChecked());
}

void ModelBrowserPanel::applyAboutStyle()
{
    const theme::Palette &p = tokens::activePalette();
    m_about->setStyleSheet(QStringLiteral(
        "QTextBrowser { background:%1; color:%2; border:1px solid %3;"
        " border-radius:%4px; padding:2px 8px; }"
        "QScrollBar:vertical { background:transparent; width:10px; }")
        .arg(p.surfaceAlt.name(), p.text.name(), p.border.name())
        .arg(p.controlRadius));

    QTextDocument *doc = m_about->document();
    doc->setDefaultStyleSheet(QStringLiteral(
        "a { color:%1; text-decoration:none; }"
        "h1 { font-size:20px; margin:8px 0 6px 0; }"
        "h2 { font-size:17px; margin:8px 0 4px 0; }"
        "h3 { font-size:15px; margin:8px 0 4px 0; }"
        "p, li { margin:2px 0; }"
        "pre, code, tt { font-family:monospace; }"
        "pre, code { background:%2; }"
        "pre { padding:6px; border-radius:%4px; }"
        "table { border-collapse:collapse; }"
        "th, td { border:1px solid %3; padding:2px 6px; }")
        .arg(p.accent.name(), p.surface.name(), p.border.name())
        .arg(p.controlRadius));
}

void ModelBrowserPanel::requestCurrentVariant()
{
    if (m_selectedId.isEmpty() || m_busy)
        return;
    const QString variantId = selectedVariantId();
    if (variantId.isEmpty())
        return;
    const QString key = m_selectedId + QLatin1Char(':') + variantId;
    // updateDetail() runs on many events (theme, busy state, patches); only a
    // genuinely new pair is worth a request.
    if (key == m_requestedVariant)
        return;
    m_requestedVariant = key;
    emit variantSelected(m_selectedId, variantId);
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
        m_specsLabel->clear();
        m_specsLabel->setVisible(false);
        m_variantCombo->clear();
        m_variantSize->clear();
        updateAbout();
        updateActionState();
        return;
    }

    m_titleLabel->setText(item->title);
    m_subtitleLabel->setText(item->subtitle);

    QString stats = item->stats;
    if (!item->about.updated.isEmpty()) {
        const QString updated = T("browser.updated").arg(localizedAgo(item->about.updated));
        stats = stats.isEmpty() ? updated
                                : stats + QStringLiteral(" · ") + updated;
    }
    m_statsLabel->setText(stats);

    // Prefer the long blurb from the family page, fall back to the search card.
    m_descriptionLabel->setText(item->description.isEmpty() ? item->about.description
                                                            : item->description);

    // Rebuild the badge chips.
    auto *badgesLayout = qobject_cast<QHBoxLayout *>(m_badgesRow->layout());
    while (QLayoutItem *child = badgesLayout->takeAt(0)) {
        if (child->widget()) {
            m_chips.removeAll(static_cast<QLabel *>(child->widget()));
            child->widget()->deleteLater();
        }
        delete child;
    }
    m_chips.clear();
    for (const QString &badge : item->badges) {
        auto *chip = new QLabel(badge);
        chip->setStyleSheet(tokens::chipStyle(badge));
        m_chips.append(chip);
        badgesLayout->addWidget(chip);
    }
    badgesLayout->addStretch();

    m_variantCombo->blockSignals(true);
    m_variantCombo->clear();
    for (const BrowserVariant &variant : item->variants) {
        m_variantCombo->addItem(
                dotIcon(variant.enabled ? variant.fit : memoryinfo::Fit::Unknown),
                variantLabel(variant));
        const int row = m_variantCombo->count() - 1;
        m_variantCombo->setItemData(row, variant.id, Qt::UserRole + 1);
        if (!variant.enabled) {
            // Grey out unsupported entries but keep them visible as an explanation.
            if (auto *model = qobject_cast<QStandardItemModel *>(m_variantCombo->model()))
                model->item(row)->setEnabled(false);
        }
    }
    applyVariantColors();
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
    updateSpecs();
    updateAbout();
    updateActionState();
    requestCurrentVariant();
}

void ModelBrowserPanel::applyVariantColors()
{
    auto *model = qobject_cast<QStandardItemModel *>(m_variantCombo->model());
    if (!model)
        return;
    const QColor muted = tokens::activePalette().muted;
    for (int row = 0; row < m_variantCombo->count(); ++row) {
        QStandardItem *entry = model->item(row);
        if (entry && !entry->isEnabled())
            m_variantCombo->setItemData(row, muted, Qt::ForegroundRole);
    }
}

void ModelBrowserPanel::updateActionState()
{
    const BrowserItem *item = itemById(m_selectedId);
    const BrowserVariant *variant = nullptr;
    if (item && item->variantsLoaded) {
        const QString id = selectedVariantId();
        for (const BrowserVariant &candidate : item->variants) {
            if (candidate.id == id) {
                variant = &candidate;
                break;
            }
        }
        if (variant)
            m_variantSize->setText(variant->hint);
    }

    // Three states of the row: nothing on disk → "Скачать" (accent, enabled);
    // local copy is current → "Скачано" (grey, disabled); local copy is stale
    // → "Обновить" (accent, enabled, pulls again).
    bool usable = variant && variant->enabled;
    bool accent = true;
    QString text = m_actionText;
    if (variant) {
        switch (variantState(variant->installed, !variant->updateAvailable)) {
        case VariantState::Installed:
            text = T("browser.downloaded");
            usable = false;
            accent = false;
            break;
        case VariantState::UpdateAvailable:
            text = T("browser.update");
            break;
        case VariantState::NotInstalled:
            break;
        }
    }

    if (m_actionBtn->property("accent").toBool() != accent) {
        m_actionBtn->setProperty("accent", accent);
        // The style sheet matches on the dynamic property; re-polish so the
        // grey "downloaded" look replaces the accent one (and back).
        m_actionBtn->style()->unpolish(m_actionBtn);
        m_actionBtn->style()->polish(m_actionBtn);
    }
    m_actionBtn->setText(text);
    m_actionBtn->setEnabled(!m_busy && usable);
    m_actionBtn->setVisible(!m_busy);
    m_cancelBtn->setVisible(m_busy && m_busyCancellable);
}

void ModelBrowserPanel::setBusy(bool busy, int percent, bool cancellable)
{
    m_busy = busy;
    m_busyCancellable = cancellable;
    m_progress->setVisible(busy);
    if (busy) {
        if (percent < 0) {
            // No total yet (e.g. `pull` is still fetching the manifest): show a
            // running bar instead of a fake 0%.
            m_progress->setRange(0, 0);
        } else {
            m_progress->setRange(0, 100);
            m_progress->setValue(qBound(0, percent, 100));
        }
    } else {
        m_progress->setRange(0, 100);
        m_progress->setValue(0);
    }
    m_list->setEnabled(!busy);
    m_variantCombo->setEnabled(!busy && itemById(m_selectedId) != nullptr);
    updateActionState();
}

void ModelBrowserPanel::setStatus(const QString &status)
{
    m_statusLabel->setText(status);
}

// Q_OBJECT classes defined in this file (A-7).
#include "modelbrowserpanel.moc"
