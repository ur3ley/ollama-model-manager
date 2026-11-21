#include "modelsearchdialog.h"

#include "designtokens.h"
#include "i18n.h"
#include "modelbrowserpanel.h"
#include "ollamacli.h"
#include "provenance.h"

#include <QCheckBox>
#include <QComboBox>
#include <QMenu>
#include <QWidgetAction>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>

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

// "121.3M" → 121.3e6, empty → 0.
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

ModelSearchDialog::ModelSearchDialog(OllamaCli *cli, QWidget *parent)
    : QDialog(parent)
    , m_cli(cli)
    , m_registry(new RegistryClient(this))
    , m_hf(new HfClient(this))
    , m_downloadDir(defaultDownloadDir())
{
    setupUi();

    connect(m_registry, &RegistryClient::searchFinished,
            this, &ModelSearchDialog::onOllamaSearchFinished);
    connect(m_registry, &RegistryClient::tagsFinished,
            this, &ModelSearchDialog::onTagsFinished);
    connect(m_cli, &OllamaCli::pullProgress, this, &ModelSearchDialog::onPullProgress);
    connect(m_cli, &OllamaCli::pullFinished, this, &ModelSearchDialog::onPullFinished);

    connect(m_hf, &HfClient::searchFinished, this, &ModelSearchDialog::onHfSearchFinished);
    connect(m_hf, &HfClient::filesFinished, this, &ModelSearchDialog::onHfFilesFinished);
    connect(m_hf, &HfClient::downloadProgress, this, &ModelSearchDialog::onHfProgress);
    connect(m_hf, &HfClient::downloadFinished, this, &ModelSearchDialog::onHfFinished);
    connect(m_registry, &RegistryClient::manifestChecked, this,
            &ModelSearchDialog::onManifestChecked);
    connect(m_hf, &HfClient::repoInfoFinished, this,
            &ModelSearchDialog::onRepoInfoFinished);
}

void ModelSearchDialog::setupUi()
{
    setWindowTitle(T("search.title"));
    resize(1080, 780);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 14, 14, 12);
    layout->setSpacing(10);

    m_tabs = new QTabWidget();
    m_tabs->setDocumentMode(true);
    layout->addWidget(m_tabs, 1);

    // ------------------------------------------------------------- Ollama tab
    auto *ollamaTab = new QWidget();
    auto *ollamaLayout = new QVBoxLayout(ollamaTab);
    ollamaLayout->setContentsMargins(0, 10, 0, 0);
    ollamaLayout->setSpacing(10);

    auto *ollamaRow = new QHBoxLayout();
    ollamaRow->setSpacing(8);
    m_queryEdit = new QLineEdit();
    m_queryEdit->setPlaceholderText(T("search.queryPlaceholder"));
    m_queryEdit->setClearButtonEnabled(true);
    ollamaRow->addWidget(m_queryEdit, 1);
    m_searchBtn = new QPushButton(T("search.search"));
    ollamaRow->addWidget(m_searchBtn);
    m_sortCombo = new QComboBox();
    m_sortCombo->addItems({T("sort.recommended"), T("sort.pulls"), T("sort.name")});
    ollamaRow->addWidget(m_sortCombo);
    m_filterBtn = new QPushButton();
    m_filterBtn->setStyleSheet(tokens::ghostButtonStyle());
    ollamaRow->addWidget(m_filterBtn);
    ollamaLayout->addLayout(ollamaRow);

    m_ollamaPanel = new ModelBrowserPanel(T("search.emptyOllama"));
    ollamaLayout->addWidget(m_ollamaPanel, 1);
    m_tabs->addTab(ollamaTab, T("search.tabOllama"));

    // -------------------------------------------------------- HuggingFace tab
    auto *hfTab = new QWidget();
    auto *hfLayout = new QVBoxLayout(hfTab);
    hfLayout->setContentsMargins(0, 10, 0, 0);
    hfLayout->setSpacing(10);

    auto *hfRow = new QHBoxLayout();
    hfRow->setSpacing(8);
    m_hfQueryEdit = new QLineEdit();
    m_hfQueryEdit->setPlaceholderText(T("search.hfPlaceholder"));
    m_hfQueryEdit->setClearButtonEnabled(true);
    hfRow->addWidget(m_hfQueryEdit, 1);
    m_hfSearchBtn = new QPushButton(T("search.search"));
    hfRow->addWidget(m_hfSearchBtn);
    m_hfSortCombo = new QComboBox();
    m_hfSortCombo->addItems({T("sort.downloads"), T("sort.newest"), T("sort.trending"),
                             T("sort.updated"), T("sort.likes")});
    hfRow->addWidget(m_hfSortCombo);
    m_filterBtnHf = new QPushButton();
    m_filterBtnHf->setStyleSheet(tokens::ghostButtonStyle());
    hfRow->addWidget(m_filterBtnHf);
    hfLayout->addLayout(hfRow);

    m_hfPanel = new ModelBrowserPanel(T("search.emptyHf"));
    m_hfPanel->setActionText(T("browser.download"));
    hfLayout->addWidget(m_hfPanel, 1);
    m_tabs->addTab(hfTab, T("search.tabHf"));

    // -------------------------------------------------------------- On device
    m_deviceTab = new QWidget();
    auto *deviceLayout = new QVBoxLayout(m_deviceTab);
    deviceLayout->setContentsMargins(0, 10, 0, 0);
    deviceLayout->setSpacing(10);

    auto *deviceRow = new QHBoxLayout();
    deviceRow->addWidget(new QLabel(T("device.hint")), 1);
    m_deviceRefreshBtn = new QPushButton(T("device.refresh"));
    deviceRow->addWidget(m_deviceRefreshBtn);
    deviceLayout->addLayout(deviceRow);

    m_devicePanel = new ModelBrowserPanel(T("device.empty"));
    m_devicePanel->setActionText(T("search.install"));
    deviceLayout->addWidget(m_devicePanel, 1);
    m_tabs->addTab(m_deviceTab, T("search.tabDevice"));

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch();
    m_closeBtn = new QPushButton(T("common.close"));
    btnRow->addWidget(m_closeBtn);
    layout->addLayout(btnRow);

    connect(m_searchBtn, &QPushButton::clicked, this, &ModelSearchDialog::onOllamaSearch);
    connect(m_queryEdit, &QLineEdit::returnPressed, this, &ModelSearchDialog::onOllamaSearch);
    connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ModelSearchDialog::onOllamaSearch);
    connect(m_filterBtn, &QPushButton::clicked, this, &ModelSearchDialog::openFilters);
    connect(m_filterBtnHf, &QPushButton::clicked, this, &ModelSearchDialog::openFilters);
    updateFilterButton();
    connect(m_ollamaPanel, &ModelBrowserPanel::itemSelected,
            this, &ModelSearchDialog::onOllamaItemSelected);
    connect(m_ollamaPanel, &ModelBrowserPanel::actionRequested,
            this, &ModelSearchDialog::onOllamaAction);
    connect(m_ollamaPanel, &ModelBrowserPanel::cancelRequested, this, [this]() {
        if (m_cli->isPulling())
            m_cli->cancelPull();
    });

    connect(m_hfSearchBtn, &QPushButton::clicked, this, &ModelSearchDialog::onHfSearch);
    connect(m_hfQueryEdit, &QLineEdit::returnPressed, this, &ModelSearchDialog::onHfSearch);
    connect(m_hfSortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ModelSearchDialog::onHfSearch);
    connect(m_hfPanel, &ModelBrowserPanel::itemSelected,
            this, &ModelSearchDialog::onHfItemSelected);
    connect(m_hfPanel, &ModelBrowserPanel::actionRequested,
            this, &ModelSearchDialog::onHfAction);
    connect(m_hfPanel, &ModelBrowserPanel::cancelRequested, this, [this]() {
        m_hf->cancelDownload();
    });

    connect(m_devicePanel, &ModelBrowserPanel::itemSelected,
            this, &ModelSearchDialog::onDeviceItemSelected);
    connect(m_devicePanel, &ModelBrowserPanel::actionRequested,
            this, &ModelSearchDialog::onDeviceAction);
    connect(m_devicePanel, &ModelBrowserPanel::cancelRequested, this, [this]() {
        if (m_cli->isPulling())
            m_cli->cancelPull();
        else
            m_hf->cancelDownload();
    });
    connect(m_deviceRefreshBtn, &QPushButton::clicked, this, &ModelSearchDialog::reloadDevice);

    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int) { ensureLoaded(); });
}

void ModelSearchDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    ensureLoaded();
}

void ModelSearchDialog::ensureLoaded()
{
    if (!m_ollamaLoaded) {
        m_ollamaLoaded = true;
        onOllamaSearch();
    }
    if (m_tabs->currentIndex() == 1 && !m_hfLoaded) {
        m_hfLoaded = true;
        onHfSearch();
    }
    if (m_tabs->currentIndex() == 2 && !m_deviceLoaded) {
        m_deviceLoaded = true;
        reloadDevice();
    }
}

// ---------------------------------------------------------------- Ollama flow

void ModelSearchDialog::applyOllamaSort(QList<RegistryClient::Model> &models) const
{
    switch (m_sortCombo->currentIndex()) {
    case 1: // pulls
        std::stable_sort(models.begin(), models.end(),
                         [](const RegistryClient::Model &a, const RegistryClient::Model &b) {
            return parseCompactNumber(a.pulls) > parseCompactNumber(b.pulls);
        });
        break;
    case 2: // name
        std::stable_sort(models.begin(), models.end(),
                         [](const RegistryClient::Model &a, const RegistryClient::Model &b) {
            return a.name.localeAwareCompare(b.name) < 0;
        });
        break;
    default:
        break; // site order ("recommended")
    }
}

void ModelSearchDialog::onOllamaSearch()
{
    if (m_cli->isPulling())
        return;
    m_ollamaPanel->clearItems();
    m_ollamaPanel->setStatus(T("search.searching"));
    m_searchBtn->setEnabled(false);
    m_registry->search(m_queryEdit->text());
}

void ModelSearchDialog::onOllamaSearchFinished(bool ok,
                                               const QList<RegistryClient::Model> &models,
                                               const QString &error)
{
    m_searchBtn->setEnabled(true);
    if (!ok) {
        m_ollamaPanel->setStatus(T("search.error").arg(error));
        return;
    }

    m_ollamaResults = models;
    rebuildOllamaItems();
}

void ModelSearchDialog::onOllamaItemSelected(const QString &itemId)
{
    m_pendingTagsId = itemId;
    m_pendingTagsFamily = itemId;
    m_tagsForDevice = false;
    refreshInstalled();
    m_ollamaPanel->setStatus(T("search.loadingTags"));
    m_registry->listTags(itemId);
}

void ModelSearchDialog::onTagsFinished(bool ok, const QList<RegistryClient::Tag> &tags,
                                       const QString &error)
{
    if (m_pendingTagsId.isEmpty())
        return;
    if (!ok) {
        (m_tagsForDevice ? m_devicePanel : m_ollamaPanel)
                ->setVariantsError(m_pendingTagsId, T("search.error").arg(error));
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
        if (!tag.context.isEmpty())
            variant.hint = T("browser.context").arg(tag.context);
        if (m_installedNames.contains(tag.name))
            variant.label += QStringLiteral("  ✓ ") + T("browser.onDevice");
        variants.append(variant);
    }

    ModelBrowserPanel *target = m_tagsForDevice ? m_devicePanel : m_ollamaPanel;
    target->setVariants(m_pendingTagsId, variants);
    if (m_tagsForDevice) {
        m_devicePanel->selectVariantById(m_deviceInstalledVariant);
        applyDeviceUpdateMark();
    } else {
        target->setStatus(variants.isEmpty() ? T("search.noTags")
                                             : T("search.tagsFound").arg(variants.size()));
    }
}

void ModelSearchDialog::onOllamaAction(const QString &, const QString &variantId)
{
    if (variantId.isEmpty() || m_cli->isPulling())
        return;
    m_activePanel = m_ollamaPanel;
    m_ollamaPanel->setBusy(true, 0);
    m_ollamaPanel->setStatus(T("search.pulling").arg(variantId));
    m_cli->pullModelAsync(variantId);
}

void ModelSearchDialog::onPullProgress(const QString &status, int percent)
{
    if (!m_activePanel)
        return;
    m_activePanel->setBusy(true, percent < 0 ? 100 : percent);
    if (!status.isEmpty())
        m_activePanel->setStatus(status);
}

void ModelSearchDialog::onPullFinished(bool ok, const QString &output)
{
    if (!m_activePanel)
        m_activePanel = m_ollamaPanel;
    m_activePanel->setBusy(false, 0);
    if (ok) {
        const QString name = m_pullingName.isEmpty() ? m_ollamaPanel->selectedVariantId() : m_pullingName;
        Provenance::Entry entry;
        entry.source = QStringLiteral("ollama");
        entry.ref = name;
        Provenance::instance().record(name, entry);
        m_activePanel->setStatus(T("search.installed").arg(name));
        m_pullingName.clear();
        if (m_deviceLoaded)
            reloadDevice();
        emit modelsChanged();
    } else {
        m_activePanel->setStatus(T("search.installFailed").arg(output));
    }
    m_activePanel = nullptr;
}

// ------------------------------------------------------------------ HF flow

QString ModelSearchDialog::hfSortKey() const
{
    switch (m_hfSortCombo->currentIndex()) {
    case 1: return QStringLiteral("createdAt");
    case 2: return QStringLiteral("trendingScore");
    case 3: return QStringLiteral("lastModified");
    case 4: return QStringLiteral("likes");
    default: return QStringLiteral("downloads");
    }
}

void ModelSearchDialog::onHfSearch()
{
    m_hfQuery = m_hfQueryEdit->text().trimmed();
    m_hfPanel->clearItems();
    m_hfPanel->setStatus(T("search.searching"));
    m_hfSearchBtn->setEnabled(false);
    m_hf->search(m_hfQuery, 40, hfSortKey());
}

void ModelSearchDialog::onHfSearchFinished(bool ok, const QList<HfClient::Repo> &repos,
                                           const QString &error)
{
    m_hfSearchBtn->setEnabled(true);
    if (!ok) {
        m_hfPanel->setStatus(T("search.error").arg(error));
        return;
    }

    m_hfResults = repos;
    m_repoShas.clear();
    for (const HfClient::Repo &repo : repos) {
        if (!repo.sha.isEmpty())
            m_repoShas.insert(repo.id, repo.sha);
    }
    rebuildHfItems();
}

void ModelSearchDialog::onHfItemSelected(const QString &itemId)
{
    m_pendingRepo = itemId;
    m_pendingFilesItemId = itemId;
    m_filesForDevice = false;
    refreshInstalled();
    m_hfPanel->setStatus(T("search.loadingFiles"));
    m_hf->listFiles(itemId);
}

void ModelSearchDialog::onHfFilesFinished(bool ok, const QList<HfClient::File> &files,
                                          const QString &error)
{
    if (m_pendingRepo.isEmpty())
        return;
    if (!ok) {
        (m_filesForDevice ? m_devicePanel : m_hfPanel)
                ->setVariantsError(m_pendingFilesItemId, T("search.error").arg(error));
        return;
    }

    QList<BrowserVariant> variants;
    variants.reserve(files.size());
    for (const HfClient::File &file : files) {
        BrowserVariant variant;
        variant.id = file.name;
        variant.label = file.quant.isEmpty() ? file.name : file.quant;
        variant.size = humanSize(file.size);
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

    ModelBrowserPanel *target = m_filesForDevice ? m_devicePanel : m_hfPanel;
    target->setVariants(m_pendingFilesItemId, variants);
    if (m_filesForDevice) {
        m_devicePanel->selectVariantById(m_deviceInstalledVariant);
        applyDeviceUpdateMark();
    }
    target->setStatus(variants.isEmpty() ? T("browser.noVariants")
                                         : T("search.filesFound").arg(variants.size()));
}

void ModelSearchDialog::onHfAction(const QString &repo, const QString &file)
{
    m_pendingRepo = repo;
    m_pendingFile = file;
    m_activePanel = m_hfPanel;
    m_hfPanel->setBusy(true, 0);
    m_hfPanel->setStatus(T("browser.downloading").arg(file));
    m_hf->download(repo, file, m_downloadDir);
}

void ModelSearchDialog::onHfProgress(qint64 received, qint64 total)
{
    if (!m_activePanel)
        return;
    const int percent = total > 0 ? int(received * 100 / total) : 0;
    m_activePanel->setBusy(true, percent);
    m_activePanel->setStatus(total > 0
                             ? T("browser.progress").arg(humanSize(received), humanSize(total))
                             : T("browser.progressNoTotal").arg(humanSize(received)));
}

void ModelSearchDialog::onHfFinished(bool ok, const QString &path, const QString &error)
{
    if (!m_activePanel)
        m_activePanel = m_hfPanel;
    m_activePanel->setBusy(false, 0);
    if (!ok) {
        m_activePanel->setStatus(T("browser.downloadFailed").arg(error));
        m_activePanel = nullptr;
        return;
    }

    const QString name = m_pendingModelName.isEmpty()
            ? HfClient::modelNameFor(m_pendingRepo, m_pendingFile)
            : m_pendingModelName;
    m_pendingModelName.clear();

    Provenance::Entry entry;
    entry.source = QStringLiteral("hf");
    entry.ref = m_pendingRepo;
    entry.file = m_pendingFile;
    entry.sha = m_repoShas.value(m_pendingRepo);
    Provenance::instance().record(name, entry);

    m_activePanel->setStatus(T("browser.downloaded").arg(name));
    emit modelCreateRequested(name, QStringLiteral("FROM ") + path + QLatin1Char('\n'));
    if (m_deviceLoaded)
        reloadDevice();
    m_activePanel = nullptr;
    emit modelsChanged();
}

// ------------------------------------------------------------------- test API

void ModelSearchDialog::searchAs(const QString &query)
{
    m_queryEdit->setText(query);
    onOllamaSearch();
}

void ModelSearchDialog::selectModel(int row)
{
    m_ollamaPanel->selectItem(row);
}

void ModelSearchDialog::selectTag(int row)
{
    m_ollamaPanel->selectVariant(row);
}

int ModelSearchDialog::modelCount() const
{
    return m_ollamaPanel->itemCount();
}

int ModelSearchDialog::tagCount() const
{
    return m_ollamaPanel->variantCount();
}

QString ModelSearchDialog::statusText() const
{
    return QString();
}

QString ModelSearchDialog::suggestedName() const
{
    return m_ollamaPanel->selectedVariantId();
}

void ModelSearchDialog::showHfTab()
{
    m_tabs->setCurrentIndex(1);
    ensureLoaded();
}

int ModelSearchDialog::hfModelCount() const
{
    return m_hfPanel->itemCount();
}

int ModelSearchDialog::hfVariantCount() const
{
    return m_hfPanel->variantCount();
}

void ModelSearchDialog::selectHfModel(int row)
{
    m_hfPanel->selectItem(row);
}

void ModelSearchDialog::startInstall()
{
    onOllamaAction(m_ollamaPanel->selectedItemId(), m_ollamaPanel->selectedVariantId());
}

// --------------------------------------------------------------- On device

void ModelSearchDialog::refreshInstalled()
{
    m_installedNames.clear();
    for (const OllamaCli::ModelInfo &model : m_cli->listModels())
        m_installedNames.insert(model.name);
}

void ModelSearchDialog::reloadDevice()
{
    const QList<OllamaCli::ModelInfo> models = m_cli->listModels();
    m_localIds.clear();
    m_installedNames.clear();

    QList<BrowserItem> items;
    items.reserve(models.size());
    for (const OllamaCli::ModelInfo &model : models) {
        m_localIds.insert(model.name, model.id);
        m_installedNames.insert(model.name);

        const Provenance::Entry prov = Provenance::instance().lookup(model.name);
        BrowserItem item;
        item.id = model.name;
        item.title = model.name;
        item.subtitle = prov.source == QLatin1String("hf")
                ? T("device.sourceHf").arg(prov.ref)
                : prov.source == QLatin1String("ollama")
                      ? T("device.sourceOllama")
                      : T("device.sourceAssumed");
        item.badges = {prov.source.isEmpty() ? QStringLiteral("unknown") : prov.source};
        item.stats = model.size + QStringLiteral(" · ") + model.modified;
        item.description = prov.installedAt.isEmpty()
                ? T("device.noProvenance")
                : T("device.installedAt").arg(prov.installedAt);

        BrowserVariant variant;
        variant.id = model.name;
        variant.label = T("device.checking");
        variant.enabled = false;
        item.variants = {variant};
        item.variantsLoaded = true;
        items.append(item);
    }

    m_devicePanel->setItems(items);
    m_devicePanel->setStatus(T("device.count").arg(items.size()));
}

void ModelSearchDialog::onDeviceItemSelected(const QString &itemId)
{
    const Provenance::Entry prov = Provenance::instance().lookup(itemId);
    refreshInstalled();
    m_deviceUpToDateKnown = false;
    m_deviceCurrentId = itemId;
    m_deviceInstalledVariant = prov.source == QLatin1String("hf") ? prov.file : itemId;
    m_devicePanel->setStatus(T("device.checking"));

    if (prov.source == QLatin1String("hf")) {
        // All quants of the same repository, so another one can be added
        // without going back to the hub.
        m_pendingRepo = prov.ref;
        m_pendingFilesItemId = itemId;
        m_filesForDevice = true;
        m_hf->listFiles(prov.ref);
        m_hf->fetchRepoInfo(prov.ref);
        return;
    }

    // Ollama models: show every tag of the same family.
    m_pendingTagsId = itemId;
    m_pendingTagsFamily = itemId.section(QLatin1Char(':'), 0, 0);
    m_tagsForDevice = true;
    m_registry->listTags(m_pendingTagsFamily);
    m_registry->checkManifest(itemId);
}

void ModelSearchDialog::setDeviceVariant(const QString &itemId, bool upToDate, bool ok,
                                         const QString &error)
{
    if (!ok) {
        m_devicePanel->setStatus(T("device.checkFailed").arg(error));
        return;
    }
    if (itemId != m_deviceCurrentId)
        return;

    m_deviceUpToDateKnown = true;
    m_deviceUpToDate = upToDate;
    applyDeviceUpdateMark();
    m_devicePanel->setStatus(upToDate ? T("device.uptodateStatus")
                                      : T("device.updateAvailable"));
}

void ModelSearchDialog::applyDeviceUpdateMark()
{
    if (!m_deviceUpToDateKnown || m_deviceInstalledVariant.isEmpty())
        return;
    // Labels already carry "✓ on device"; only the freshness suffix is added.
    m_devicePanel->patchVariant(m_deviceCurrentId, m_deviceInstalledVariant,
                                m_deviceUpToDate ? QStringLiteral("  ·  ") + T("device.uptodate")
                                                 : QStringLiteral("  ·  ") + T("device.update"));
}

void ModelSearchDialog::onManifestChecked(const QString &ref, const QString &digest, bool ok,
                                          const QString &error)
{
    const QString localId = m_localIds.value(ref);
    const bool upToDate = ok && !localId.isEmpty() && digest.startsWith(localId.left(12));
    setDeviceVariant(ref, upToDate, ok, error);
}

void ModelSearchDialog::onRepoInfoFinished(const QString &repo, const QString &sha, bool ok,
                                           const QString &error)
{
    QString modelName;
    for (auto it = m_localIds.constBegin(); it != m_localIds.constEnd(); ++it) {
        const Provenance::Entry prov = Provenance::instance().lookup(it.key());
        if (prov.source == QLatin1String("hf") && prov.ref == repo) {
            modelName = it.key();
            break;
        }
    }
    if (modelName.isEmpty())
        return;
    const bool upToDate = ok && !sha.isEmpty()
            && sha == Provenance::instance().lookup(modelName).sha;
    setDeviceVariant(modelName, upToDate, ok, error);
}

void ModelSearchDialog::onDeviceAction(const QString &itemId, const QString &variantId)
{
    const Provenance::Entry prov = Provenance::instance().lookup(itemId);
    m_activePanel = m_devicePanel;

    if (prov.source == QLatin1String("hf") && !prov.ref.isEmpty()) {
        // `variantId` may be a different quant of the same repository.
        m_pendingRepo = prov.ref;
        m_pendingFile = variantId.isEmpty() ? prov.file : variantId;
        m_pendingModelName = m_pendingFile == prov.file
                ? itemId
                : HfClient::modelNameFor(prov.ref, m_pendingFile);
        m_devicePanel->setBusy(true, 0);
        m_devicePanel->setStatus(T("browser.downloading").arg(m_pendingFile));
        m_hf->download(prov.ref, m_pendingFile, m_downloadDir);
        return;
    }

    const QString target = variantId.isEmpty() ? itemId : variantId;
    m_pullingName = target;
    m_devicePanel->setBusy(true, 0);
    m_devicePanel->setStatus(T("search.pulling").arg(target));
    m_cli->pullModelAsync(target);
}

void ModelSearchDialog::showDeviceTab()
{
    m_tabs->setCurrentIndex(2);
    ensureLoaded();
}

int ModelSearchDialog::deviceCount() const
{
    return m_devicePanel->itemCount();
}

int ModelSearchDialog::deviceVariantCount() const
{
    return m_devicePanel->variantCount();
}

void ModelSearchDialog::selectDevice(int row)
{
    m_devicePanel->selectItem(row);
}

// ------------------------------------------------------------------ filters

void ModelSearchDialog::rebuildOllamaItems()
{
    QList<RegistryClient::Model> sorted = m_ollamaResults;
    applyOllamaSort(sorted);

    QList<BrowserItem> items;
    items.reserve(sorted.size());
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
        item.variantsLoaded = false;
        items.append(item);
    }

    m_ollamaPanel->setItems(items);
    m_ollamaPanel->setStatus(items.isEmpty() ? T("search.nothingFound")
                                             : T("search.modelsFound").arg(items.size()));
}

void ModelSearchDialog::rebuildHfItems()
{
    const QString uploader = m_filters.uploader.trimmed().toLower();

    QList<BrowserItem> items;
    items.reserve(m_hfResults.size());
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
        item.variantsLoaded = false;
        items.append(item);
    }

    m_hfPanel->setItems(items);
    m_hfPanel->setStatus(items.isEmpty() ? T("search.nothingFound")
                                         : T("search.modelsFound").arg(items.size()));
}

void ModelSearchDialog::updateFilterButton()
{
    int active = m_filters.capabilities.size();
    if (m_filters.maxParamsB > 0)
        ++active;
    if (!m_filters.uploader.trimmed().isEmpty())
        ++active;
    const QString text = active > 0 ? T("filters.buttonActive").arg(active)
                                    : T("filters.button");
    m_filterBtn->setText(text);
    m_filterBtnHf->setText(text);
}

void ModelSearchDialog::openFilters()
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
            rebuildOllamaItems();
            rebuildHfItems();
        });
        layout->addWidget(box);
    }

    auto *sizeTitle = new QLabel(T("filters.size"));
    sizeTitle->setStyleSheet(QStringLiteral("color:%1; font-size:11px; font-weight:600;")
                                     .arg(tokens::Muted));
    layout->addWidget(sizeTitle);

    auto *sizeCombo = new QComboBox();
    sizeCombo->addItem(T("filters.any"), 0);
    for (int limit : {4, 8, 14, 32, 70, 128})
        sizeCombo->addItem(T("filters.upTo").arg(limit), limit);
    const int index = sizeCombo->findData(m_filters.maxParamsB);
    sizeCombo->setCurrentIndex(index >= 0 ? index : 0);
    connect(sizeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, sizeCombo](int) {
        m_filters.maxParamsB = sizeCombo->currentData().toInt();
        updateFilterButton();
        rebuildOllamaItems();
        rebuildHfItems();
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
        rebuildHfItems();
    });
    layout->addWidget(uploaderEdit);

    auto *reset = new QPushButton(T("filters.reset"));
    reset->setStyleSheet(tokens::ghostButtonStyle());
    connect(reset, &QPushButton::clicked, this, [this, &menu]() {
        m_filters = ModelFilters();
        updateFilterButton();
        rebuildOllamaItems();
        rebuildHfItems();
        menu.close();
    });
    layout->addWidget(reset);

    widget->setMinimumWidth(260);
    auto *action = new QWidgetAction(&menu);
    action->setDefaultWidget(widget);
    menu.addAction(action);
    menu.exec(m_filterBtn->mapToGlobal(QPoint(0, m_filterBtn->height())));
}
