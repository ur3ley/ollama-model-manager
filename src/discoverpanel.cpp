#include "discoverpanel.h"

#include "designtokens.h"
#include "i18n.h"
#include "memoryinfo.h"
#include "modelbrowserpanel.h"
#include "ollamacli.h"
#include "provenance.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>

namespace {

QString humanSize(qint64 bytes)
{
    if (bytes <= 0)
        return QStringLiteral("—");
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QString::number(value, 'f', value < 10.0 ? 1 : 0)
            + QLatin1Char(' ') + QLatin1String(units[unit]);
}

QString shortNumber(qint64 value)
{
    if (value >= 1000000000)
        return QString::number(value / 1e9, 'f', 1) + QStringLiteral("B");
    if (value >= 1000000)
        return QString::number(value / 1e6, 'f', 1) + QStringLiteral("M");
    if (value >= 1000)
        return QString::number(value / 1e3, 'f', 1) + QStringLiteral("K");
    return QString::number(value);
}

double parseCompactNumber(const QString &text)
{
    if (text.isEmpty())
        return 0.0;
    QChar last = text.back().toUpper();
    double multiplier = 1.0;
    QString number = text;
    if (last == QLatin1Char('K')) {
        multiplier = 1e3;
        number.chop(1);
    } else if (last == QLatin1Char('M')) {
        multiplier = 1e6;
        number.chop(1);
    } else if (last == QLatin1Char('B')) {
        multiplier = 1e9;
        number.chop(1);
    }
    bool ok = false;
    const double value = number.toDouble(&ok);
    return ok ? value * multiplier : 0.0;
}

QString defaultDownloadDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return (base.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::HomeLocation) : base)
            + QStringLiteral("/models");
}

bool isProjector(const QString &name)
{
    return name.contains(QLatin1String("mmproj"), Qt::CaseInsensitive);
}

} // namespace

DiscoverPanel::DiscoverPanel(Source source, OllamaCli *cli, QWidget *parent)
    : QWidget(parent)
    , m_source(source)
    , m_cli(cli)
    , m_registry(new RegistryClient(this))
    , m_hf(new HfClient(this))
{
    setupUi();

    connect(m_registry, &RegistryClient::searchFinished, this, [this](bool ok,
            const QList<RegistryClient::Model> &models, const QString &error) {
        m_searchBtn->setEnabled(true);
        if (!ok) {
            m_panel->setStatus(T("search.error").arg(error));
            return;
        }
        m_ollamaResults = models;
        rebuildItems();
    });
    connect(m_registry, &RegistryClient::tagsFinished, this, [this](bool ok,
            const QList<RegistryClient::Tag> &tags, const QString &error) {
        if (m_pendingItemId.isEmpty())
            return;
        if (!ok) {
            m_panel->setVariantsError(m_pendingItemId, T("search.error").arg(error));
            return;
        }
        QList<BrowserVariant> variants;
        variants.reserve(tags.size());
        for (const RegistryClient::Tag &tag : tags) {
            BrowserVariant variant;
            variant.id = tag.name;
            const int colon = tag.name.indexOf(QLatin1Char(':'));
            variant.label = colon > 0 ? tag.name.mid(colon + 1) : tag.name;
            variant.size = tag.size;
            variant.sizeBytes = memoryinfo::parseSizeBytes(tag.size);
            variant.fitsMemory = memoryinfo::fits(variant.sizeBytes);
            if (!tag.context.isEmpty())
                variant.hint = T("browser.context").arg(tag.context);
            if (m_installedNames.contains(tag.name))
                variant.label += QStringLiteral("  ✓ ") + T("browser.onDevice");
            variants.append(variant);
        }
        m_panel->setVariants(m_pendingItemId, variants);
        m_panel->setStatus(variants.isEmpty() ? T("search.noTags")
                                              : T("search.tagsFound").arg(variants.size()));
    });

    connect(m_hf, &HfClient::searchFinished, this, [this](bool ok,
            const QList<HfClient::Repo> &repos, const QString &error) {
        m_searchBtn->setEnabled(true);
        if (!ok) {
            m_panel->setStatus(T("search.error").arg(error));
            return;
        }
        m_hfResults = repos;
        rebuildItems();
    });
    connect(m_hf, &HfClient::filesFinished, this, [this](bool ok,
            const QList<HfClient::File> &files, const QString &error) {
        if (m_pendingItemId.isEmpty())
            return;
        if (!ok) {
            m_panel->setVariantsError(m_pendingItemId, T("search.error").arg(error));
            return;
        }
        QList<BrowserVariant> variants;
        variants.reserve(files.size());
        for (const HfClient::File &file : files) {
            BrowserVariant variant;
            variant.id = file.name;
            variant.label = file.quant.isEmpty() ? file.name : file.quant;
            variant.size = humanSize(file.size);
            variant.sizeBytes = quint64(file.size);
            variant.fitsMemory = memoryinfo::fits(variant.sizeBytes);
            if (isProjector(file.name)) {
                variant.enabled = false;
                variant.hint = T("browser.projector");
            } else if (file.split) {
                variant.enabled = false;
                variant.hint = T("browser.split").arg(file.parts);
            }
            if (!variant.hint.isEmpty())
                variant.label += QStringLiteral("  ·  ") + variant.hint;
            const QString localName = HfClient::modelNameFor(m_pendingRepo, file.name);
            if (m_installedNames.contains(localName))
                variant.label += QStringLiteral("  ✓ ") + T("browser.onDevice");
            variants.append(variant);
        }
        m_panel->setVariants(m_pendingItemId, variants);
        m_panel->setStatus(variants.isEmpty() ? T("browser.noVariants")
                                              : T("search.filesFound").arg(variants.size()));
    });

    connect(m_cli, &OllamaCli::pullProgress, this, &DiscoverPanel::onPullProgress);
    connect(m_cli, &OllamaCli::pullFinished, this, &DiscoverPanel::onPullFinished);
    connect(m_hf, &HfClient::downloadProgress, this, &DiscoverPanel::onDownloadProgress);
    connect(m_hf, &HfClient::downloadFinished, this, &DiscoverPanel::onDownloadFinished);
}

void DiscoverPanel::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(10);

    auto *row = new QHBoxLayout();
    row->setSpacing(8);
    m_queryEdit = new QLineEdit();
    m_queryEdit->setPlaceholderText(isOllama() ? T("search.queryPlaceholder")
                                               : T("search.hfPlaceholder"));
    m_queryEdit->setClearButtonEnabled(true);
    row->addWidget(m_queryEdit, 1);
    m_searchBtn = new QPushButton(T("search.search"));
    row->addWidget(m_searchBtn);

    if (isOllama()) {
        m_sortCombo = new QComboBox();
        m_sortCombo->setStyleSheet(tokens::comboStyle());
        m_sortCombo->addItems({T("sort.recommended"), T("sort.pulls"), T("sort.name")});
        row->addWidget(m_sortCombo);
        connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DiscoverPanel::onSearch);
    } else {
        m_hfSortCombo = new QComboBox();
        m_hfSortCombo->setStyleSheet(tokens::comboStyle());
        m_hfSortCombo->addItems({T("sort.downloads"), T("sort.newest"), T("sort.trending"),
                                 T("sort.updated"), T("sort.likes")});
        row->addWidget(m_hfSortCombo);
        connect(m_hfSortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DiscoverPanel::onSearch);
    }

    m_filterBtn = new QPushButton();
    m_filterBtn->setStyleSheet(tokens::ghostButtonStyle());
    row->addWidget(m_filterBtn);
    layout->addLayout(row);

    m_panel = new ModelBrowserPanel(isOllama() ? T("search.emptyOllama") : T("search.emptyHf"));
    m_panel->setActionText(isOllama() ? T("browser.download") : T("browser.download"));
    layout->addWidget(m_panel, 1);

    connect(m_searchBtn, &QPushButton::clicked, this, &DiscoverPanel::onSearch);
    connect(m_queryEdit, &QLineEdit::returnPressed, this, &DiscoverPanel::onSearch);
    connect(m_filterBtn, &QPushButton::clicked, this, &DiscoverPanel::openFilters);
    connect(m_panel, &ModelBrowserPanel::itemSelected, this, &DiscoverPanel::onItemSelected);
    connect(m_panel, &ModelBrowserPanel::actionRequested, this, &DiscoverPanel::onAction);
    connect(m_panel, &ModelBrowserPanel::cancelRequested, this, &DiscoverPanel::onCancel);

    updateFilterButton();
}

void DiscoverPanel::activate()
{
    if (m_loaded)
        return;
    m_loaded = true;
    onSearch();
}

void DiscoverPanel::onSearch()
{
    m_panel->clearItems();
    m_panel->setStatus(T("search.searching"));
    m_searchBtn->setEnabled(false);
    if (isOllama())
        m_registry->search(m_queryEdit->text());
    else
        m_hf->search(m_queryEdit->text().trimmed(), 40, hfSortKey());
}

QString DiscoverPanel::hfSortKey() const
{
    switch (m_hfSortCombo ? m_hfSortCombo->currentIndex() : 0) {
    case 1: return QStringLiteral("createdAt");
    case 2: return QStringLiteral("trendingScore");
    case 3: return QStringLiteral("lastModified");
    case 4: return QStringLiteral("likes");
    default: return QStringLiteral("downloads");
    }
}

void DiscoverPanel::applySort(QList<RegistryClient::Model> &models) const
{
    if (!m_sortCombo)
        return;
    switch (m_sortCombo->currentIndex()) {
    case 1:
        std::stable_sort(models.begin(), models.end(),
                         [](const RegistryClient::Model &a, const RegistryClient::Model &b) {
            return parseCompactNumber(a.pulls) > parseCompactNumber(b.pulls);
        });
        break;
    case 2:
        std::stable_sort(models.begin(), models.end(),
                         [](const RegistryClient::Model &a, const RegistryClient::Model &b) {
            return a.name.localeAwareCompare(b.name) < 0;
        });
        break;
    default:
        break;
    }
}

void DiscoverPanel::rebuildItems()
{
    QList<BrowserItem> items;

    if (isOllama()) {
        QList<RegistryClient::Model> sorted = m_ollamaResults;
        applySort(sorted);
        for (const RegistryClient::Model &model : sorted) {
            if (!filterlogic::hasCapabilities(model.capabilities, m_filters.capabilities))
                continue;
            if (!filterlogic::fitsMaxParams(model.sizes, m_filters.maxParamsB))
                continue;

            BrowserItem item;
            item.id = model.name;
            item.title = model.name;
            item.subtitle = QStringLiteral("ollama.com");
            item.description = model.description;
            item.badges = model.capabilities;
            item.stats = model.pulls.isEmpty()
                    ? model.sizes.join(QStringLiteral(" · "))
                    : T("browser.pulls").arg(model.pulls);
            items.append(item);
        }
    } else {
        const QString uploader = m_filters.uploader.trimmed().toLower();
        for (const HfClient::Repo &repo : m_hfResults) {
            const QStringList caps = filterlogic::hfTagsToCapabilities(repo.tags);
            if (!filterlogic::hasCapabilities(caps, m_filters.capabilities))
                continue;
            if (!uploader.isEmpty() && !repo.id.toLower().contains(uploader))
                continue;

            BrowserItem item;
            item.id = repo.id;
            const int slash = repo.id.indexOf(QLatin1Char('/'));
            item.title = slash >= 0 ? repo.id.mid(slash + 1) : repo.id;
            item.subtitle = slash >= 0 ? repo.id.left(slash) : QString();
            item.badges = QStringList{QStringLiteral("gguf")} + caps;
            item.stats = T("browser.stats")
                    .arg(shortNumber(repo.downloads), shortNumber(repo.likes));
            items.append(item);
        }
    }

    m_panel->setItems(items);
    m_panel->setStatus(items.isEmpty() ? T("search.nothingFound")
                                       : T("search.modelsFound").arg(items.size()));
}

void DiscoverPanel::refreshInstalled()
{
    m_installedNames.clear();
    for (const OllamaCli::ModelInfo &model : m_cli->listModels())
        m_installedNames.insert(model.name);
}

void DiscoverPanel::onItemSelected(const QString &itemId)
{
    m_pendingItemId = itemId;
    refreshInstalled();
    if (isOllama()) {
        m_panel->setStatus(T("search.loadingTags"));
        m_registry->listTags(itemId);
    } else {
        m_pendingRepo = itemId;
        m_panel->setStatus(T("search.loadingFiles"));
        m_hf->listFiles(itemId);
    }
}

void DiscoverPanel::onAction(const QString &itemId, const QString &variantId)
{
    m_panel->setBusy(true, 0);
    if (isOllama()) {
        if (variantId.isEmpty() || m_cli->isPulling())
            return;
        m_pullingName = variantId;
        m_panel->setStatus(T("search.pulling").arg(variantId));
        m_cli->pullModelAsync(variantId);
        return;
    }

    m_pendingRepo = itemId;
    m_pendingFile = variantId;
    m_panel->setStatus(T("browser.downloading").arg(variantId));
    m_hf->download(itemId, variantId, defaultDownloadDir());
}

void DiscoverPanel::onCancel()
{
    if (isOllama()) {
        if (m_cli->isPulling())
            m_cli->cancelPull();
    } else {
        m_hf->cancelDownload();
    }
}

void DiscoverPanel::onPullProgress(const QString &status, int percent)
{
    m_panel->setBusy(true, percent < 0 ? 100 : percent);
    if (!status.isEmpty())
        m_panel->setStatus(status);
}

void DiscoverPanel::onPullFinished(bool ok, const QString &output)
{
    m_panel->setBusy(false, 0);
    if (!ok) {
        m_panel->setStatus(T("search.installFailed").arg(output));
        return;
    }

    Provenance::Entry entry;
    entry.source = QStringLiteral("ollama");
    entry.ref = m_pullingName;
    Provenance::instance().record(m_pullingName, entry);

    m_panel->setStatus(T("search.installed").arg(m_pullingName));
    m_pullingName.clear();
    refreshInstalled();
    emit installedChanged();
}

void DiscoverPanel::onDownloadProgress(qint64 received, qint64 total)
{
    const int percent = total > 0 ? int(received * 100 / total) : 0;
    m_panel->setBusy(true, percent);
    m_panel->setStatus(total > 0
                       ? T("browser.progress").arg(humanSize(received), humanSize(total))
                       : T("browser.progressNoTotal").arg(humanSize(received)));
}

void DiscoverPanel::onDownloadFinished(bool ok, const QString &path, const QString &error)
{
    m_panel->setBusy(false, 0);
    if (!ok) {
        m_panel->setStatus(T("browser.downloadFailed").arg(error));
        return;
    }

    const QString name = HfClient::modelNameFor(m_pendingRepo, m_pendingFile);
    Provenance::Entry entry;
    entry.source = QStringLiteral("hf");
    entry.ref = m_pendingRepo;
    entry.file = m_pendingFile;
    Provenance::instance().record(name, entry);

    m_panel->setStatus(T("browser.downloaded").arg(name));
    emit modelCreateRequested(name, QStringLiteral("FROM ") + path + QLatin1Char('\n'));
    emit installedChanged();
}

void DiscoverPanel::updateFilterButton()
{
    int active = m_filters.capabilities.size();
    if (m_filters.maxParamsB > 0)
        ++active;
    if (!m_filters.uploader.trimmed().isEmpty())
        ++active;
    m_filterBtn->setText(active > 0 ? T("filters.buttonActive").arg(active)
                                    : T("filters.button"));
}

void DiscoverPanel::openFilters()
{
    QMenu menu(this);
    auto *widget = new QWidget();
    auto *layout = new QVBoxLayout(widget);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    auto *title = new QLabel(T("filters.capabilities"));
    title->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:600;")
                                 .arg(tokens::Muted));
    layout->addWidget(title);

    struct Capability {
        QString id;
        QString label;
    };
    const QList<Capability> caps = {
        {QStringLiteral("tools"), T("filters.tools")},
        {QStringLiteral("thinking"), T("filters.thinking")},
        {QStringLiteral("vision"), T("filters.vision")},
        {QStringLiteral("audio"), T("filters.audio")},
        {QStringLiteral("embedding"), T("filters.embedding")},
        {QStringLiteral("imagegen"), T("filters.imagegen")},
        {QStringLiteral("videogen"), T("filters.videogen")},
    };
    for (const Capability &cap : caps) {
        auto *box = new QCheckBox(cap.label);
        box->setChecked(m_filters.capabilities.contains(cap.id));
        connect(box, &QCheckBox::toggled, this, [this, cap](bool on) {
            if (on && !m_filters.capabilities.contains(cap.id))
                m_filters.capabilities.append(cap.id);
            else if (!on)
                m_filters.capabilities.removeAll(cap.id);
            updateFilterButton();
            rebuildItems();
        });
        layout->addWidget(box);
    }

    auto *sizeTitle = new QLabel(T("filters.size"));
    sizeTitle->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:600;")
                                     .arg(tokens::Muted));
    layout->addWidget(sizeTitle);

    auto *sizeCombo = new QComboBox();
    sizeCombo->setStyleSheet(tokens::comboStyle());
    sizeCombo->addItem(T("filters.any"), 0);
    for (int limit : {4, 8, 14, 32, 70, 128})
        sizeCombo->addItem(T("filters.upTo").arg(limit), limit);
    const int index = sizeCombo->findData(m_filters.maxParamsB);
    sizeCombo->setCurrentIndex(index >= 0 ? index : 0);
    connect(sizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, sizeCombo](int) {
        m_filters.maxParamsB = sizeCombo->currentData().toInt();
        updateFilterButton();
        rebuildItems();
    });
    layout->addWidget(sizeCombo);

    auto *uploaderTitle = new QLabel(T("filters.uploader"));
    uploaderTitle->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:600;")
                                         .arg(tokens::Muted));
    layout->addWidget(uploaderTitle);
    auto *uploaderEdit = new QLineEdit(m_filters.uploader);
    uploaderEdit->setPlaceholderText(T("filters.uploaderPlaceholder"));
    connect(uploaderEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_filters.uploader = text;
        updateFilterButton();
        rebuildItems();
    });
    layout->addWidget(uploaderEdit);

    auto *reset = new QPushButton(T("filters.reset"));
    reset->setStyleSheet(tokens::ghostButtonStyle());
    connect(reset, &QPushButton::clicked, this, [this, &menu]() {
        m_filters = ModelFilters();
        updateFilterButton();
        rebuildItems();
        menu.close();
    });
    layout->addWidget(reset);

    widget->setMinimumWidth(260);
    auto *action = new QWidgetAction(&menu);
    action->setDefaultWidget(widget);
    menu.addAction(action);
    menu.exec(m_filterBtn->mapToGlobal(QPoint(0, m_filterBtn->height())));
}

// ------------------------------------------------------------------ test API

void DiscoverPanel::searchAs(const QString &query)
{
    m_loaded = true;
    m_queryEdit->setText(query);
    onSearch();
}

void DiscoverPanel::selectModel(int row)
{
    m_panel->selectItem(row);
}

void DiscoverPanel::selectTag(int row)
{
    m_panel->selectVariant(row);
}

int DiscoverPanel::modelCount() const
{
    return m_panel->itemCount();
}

int DiscoverPanel::tagCount() const
{
    return m_panel->variantCount();
}

QString DiscoverPanel::suggestedName() const
{
    return m_panel->selectedVariantId();
}

QString DiscoverPanel::statusText() const
{
    return QString();
}

void DiscoverPanel::startInstall()
{
    onAction(m_panel->selectedItemId(), m_panel->selectedVariantId());
}
