#include "discoverpanel.h"

#include "designtokens.h"
#include "combohighlightdelegate.h"
#include "hfauth.h"
#include "hftokendialog.h"
#include "hfvariants.h"
#include "i18n.h"
#include "memoryinfo.h"
#include "modelbrowserpanel.h"
#include "ollamacli.h"
#include "popupcombobox.h"
#include "provenance.h"
#include "pullprogress.h"
#include "quantmatch.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QStandardPaths>
#include <QStorageInfo>
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

// Bytes free on the filesystem that holds `dir`; 0 when it cannot be queried
// (then the caller skips the check instead of blocking the download).
quint64 freeDiskBytes(const QString &dir)
{
    if (!QDir().mkpath(dir))
        return 0;
    const QStorageInfo info(dir);
    if (!info.isValid() || !info.isReady())
        return 0;
    return quint64(qMax<qint64>(info.bytesAvailable(), 0));
}

// Download + temporary `.part` overhead: 5% of the model plus a small floor.
quint64 requiredSpaceBytes(qint64 modelBytes)
{
    const quint64 bytes = quint64(qMax<qint64>(modelBytes, 0));
    return bytes + bytes / 20 + (64ull << 20);
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
        QStringList installedRefs;
        for (const RegistryClient::Tag &tag : tags) {
            BrowserVariant variant;
            variant.id = tag.name;
            const int colon = tag.name.indexOf(QLatin1Char(':'));
            variant.label = colon > 0 ? tag.name.mid(colon + 1) : tag.name;
            variant.size = tag.size;
            variant.context = tag.context;
            variant.input = tag.input;
            variant.sizeBytes = memoryinfo::parseSizeBytes(tag.size);
            variant.fit = memoryinfo::fitLevel(variant.sizeBytes);
            if (!tag.context.isEmpty())
                variant.hint = T("browser.context").arg(tag.context);
            variant.installed = m_installedNames.contains(tag.name);
            if (variant.installed)
                installedRefs.append(tag.name);
            variants.append(variant);
        }
        m_panel->setVariants(m_pendingItemId, variants);
        m_panel->setStatus(variants.isEmpty() ? T("search.noTags")
                                              : T("search.tagsFound").arg(variants.size()));
        // Only installed tags can be stale; the check runs after the list is
        // shown, one manifest at a time, so the UI never waits for it.
        checkOllamaUpdates(m_pendingItemId, installedRefs);
    });

    connect(m_registry, &RegistryClient::modelDetailsReady, this,
            [this](const QString &family, const RegistryClient::ModelDetails &details, bool ok) {
        if (!isOllama() || family != m_pendingItemId || !ok)
            return;
        BrowserAbout about;
        about.description = details.description;
        about.readmeMarkdown = details.readmeMarkdown;
        about.updated = details.updated;
        m_panel->setModelDetails(family, about);
    });

    connect(m_registry, &RegistryClient::tagDetailsReady, this,
            [this](const QString &ref, const RegistryClient::TagDetails &details, bool ok) {
        if (!isOllama() || !ok || ref != m_panel->selectedVariantId())
            return;
        // The panel keeps the facts per variant, so a reply that arrives after
        // the user picked another quant is stored but not shown.
        BrowserSpecs specs;
        specs.architecture = details.architecture;
        specs.parameters = details.parameters;
        specs.quantization = details.quantization;
        specs.license = details.license;
        m_panel->setTagDetails(m_pendingItemId, ref, specs);
    });

    connect(m_panel, &ModelBrowserPanel::variantSelected, this,
            [this](const QString &itemId, const QString &variantId) {
        if (!isOllama() || itemId.isEmpty() || variantId.isEmpty())
            return;
        m_pendingItemId = itemId;
        m_registry->fetchTagDetails(variantId);
    });

    // Update check reply: turn the registry digest into the variant's mark.
    // The queue is single-file (RegistryClient keeps one manifest reply), so
    // only the ref at its head is accepted; anything else belongs to a family
    // the user has already left.
    connect(m_registry, &RegistryClient::manifestChecked, this,
            [this](const QString &ref, const QString &digest, bool ok, const QString &) {
        if (m_updateQueue.isEmpty() || m_updateQueue.first() != ref)
            return;
        m_updateQueue.removeFirst();
        if (ok && !digest.isEmpty())
            m_manifestDigests.insert(ref, digest);
        applyOllamaState(m_updateItemId, ref);
        startNextUpdateCheck();
    });

    connect(m_hf, &HfClient::searchFinished, this, [this](bool ok,
            const QList<HfClient::Repo> &repos, const QString &error) {
        m_searchBtn->setEnabled(true);
        if (!ok) {
            m_panel->setStatus(T("search.error").arg(hfErrorText(error)));
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
            m_panel->setVariantsError(m_pendingItemId, T("search.error").arg(hfErrorText(error)));
            return;
        }
        // One entry per quantization: split GGUF parts are folded into a
        // single variant instead of flooding the picker. Ollama imports a
        // split model from a wildcard, so those entries are downloadable too.
        m_hfVariants = groupHfFiles(files);
        applyHfVariants(m_pendingItemId);
    });
    // Repository commit: the basis of the "обновить" mark for installed GGUFs.
    connect(m_hf, &HfClient::repoInfoFinished, this,
            [this](const QString &repo, const QString &sha, bool ok, const QString &) {
        if (!ok || sha.isEmpty()) {
            // No comparable revision: never fake an update, retry on a revisit.
            m_repoInfoRequested.remove(repo);
            return;
        }
        m_repoShas.insert(repo, sha);
        // In-place marks only: rebuilding the picker here would drop the file
        // the user is looking at.
        applyHfStates(repo);
    });

    // A downloaded GGUF is only usable after `ollama create` finished; only
    // then does the variant get its "on device" mark.
    connect(m_cli, &OllamaCli::createFinished, this,
            [this](bool ok, const QString &output) {
        if (m_pendingCreateName.isEmpty())
            return;
        const QString name = m_pendingCreateName;
        m_pendingCreateName.clear();
        m_panel->setBusy(false, 0);
        if (!ok) {
            m_panel->setStatus(T("browser.createFailed").arg(output));
            return;
        }

        Provenance::Entry entry;
        entry.source = QStringLiteral("hf");
        entry.ref = m_pendingCreateRepo;
        entry.file = m_pendingCreateFile;
        // The commit the file came from is the baseline of the next update
        // check; unknown (repo info failed) leaves the field empty, which the
        // comparison treats as "no update".
        entry.sha = m_repoShas.value(m_pendingCreateRepo);
        Provenance::instance().record(name, entry);

        // Ollama has copied the shards into its blob store: the downloaded
        // sources are now a second full copy, so drop them.
        quint64 freed = 0;
        for (const QString &path : std::as_const(m_pendingCreateSources)) {
            const qint64 size = QFileInfo(path).size();
            if (QFile::remove(path))
                freed += quint64(qMax<qint64>(size, 0));
            QFile::remove(path + QStringLiteral(".part"));
        }
        m_pendingCreateSources.clear();

        refreshInstalled();
        if (!m_pendingItemId.isEmpty())
            applyHfVariants(m_pendingItemId);
        m_panel->setStatus(freed > 0
                ? T("browser.installedCleaned").arg(name, humanSize(qint64(freed)))
                : T("search.installed").arg(name));
        emit installedChanged();
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
        m_sortCombo = new PopupComboBox();
        m_sortCombo->view()->setItemDelegate(
                new ComboHighlightDelegate(m_sortCombo->view()));
        m_sortCombo->addItems({T("sort.recommended"), T("sort.pulls"), T("sort.name")});
        row->addWidget(m_sortCombo);
        connect(m_sortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DiscoverPanel::onSearch);
    } else {
        m_hfSortCombo = new PopupComboBox();
        m_hfSortCombo->view()->setItemDelegate(
                new ComboHighlightDelegate(m_hfSortCombo->view()));
        m_hfSortCombo->addItems({T("sort.downloads"), T("sort.newest"), T("sort.trending"),
                                 T("sort.updated"), T("sort.likes")});
        row->addWidget(m_hfSortCombo);
        connect(m_hfSortCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &DiscoverPanel::onSearch);
    }

    m_filterBtn = new QPushButton();
    row->addWidget(m_filterBtn);
    layout->addLayout(row);

    m_panel = new ModelBrowserPanel(isOllama() ? T("search.emptyOllama") : T("search.emptyHf"));
    m_panel->setObjectName(isOllama() ? QStringLiteral("ollamaBrowserPanel")
                                      : QStringLiteral("hfBrowserPanel"));
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

void DiscoverPanel::focusSearch()
{
    m_queryEdit->setFocus(Qt::OtherFocusReason);
    m_queryEdit->selectAll();
}

void DiscoverPanel::onSearch()
{
    m_panel->clearItems();
    m_panel->setStatus(T("search.searching"));
    m_searchBtn->setEnabled(false);
    if (isOllama()) {
        m_registry->search(m_queryEdit->text());
    } else {
        ensureHfToken();
        m_hf->search(m_queryEdit->text().trimmed(), 40, hfSortKey());
    }
}

/**
 * First HuggingFace request in this session: pick up a token the wallet already
 * holds. HfAuth::token() is passive (no password prompt), so this cannot stop
 * browsing — and a locked wallet simply means the 401 hint path later.
 */
void DiscoverPanel::ensureHfToken()
{
    if (m_tokenLoaded)
        return;
    m_tokenLoaded = true;
    m_hf->setToken(HfAuth::token());
}

QString DiscoverPanel::hfErrorText(const QString &error) const
{
    if (!HfClient::isAuthError(error))
        return error;
    return T("hf.token.error").arg(error);
}

void DiscoverPanel::openTokenDialog()
{
    HfTokenDialog dialog(m_hf, this);
    dialog.exec();
    // The dialog applies the token to m_hf itself, so the next attempt uses it
    // without a restart; say so unless the user just closed the window.
    if (dialog.changed() && m_hf->hasToken())
        m_panel->setStatus(T("hf.token.applied"));
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
    m_localIds.clear();
    for (const OllamaCli::ModelInfo &model : m_cli->listModels()) {
        m_installedNames.insert(model.name);
        // The id is the manifest digest `ollama list` printed: the local side
        // of the update comparison.
        m_localIds.insert(model.name, model.id);
    }
}

void DiscoverPanel::onItemSelected(const QString &itemId)
{
    m_pendingItemId = itemId;
    refreshInstalled();
    if (isOllama()) {
        m_panel->setStatus(T("search.loadingTags"));
        m_registry->listTags(itemId);
        m_registry->fetchModelDetails(itemId);
    } else {
        ensureHfToken();
        m_pendingRepo = itemId;
        m_panel->setStatus(T("search.loadingFiles"));
        m_hf->listFiles(itemId);
        ensureRepoInfo(itemId);
    }
}

void DiscoverPanel::onAction(const QString &itemId, const QString &variantId)
{
    if (isOllama()) {
        if (variantId.isEmpty() || m_cli->isPulling())
            return;
        m_pullingName = variantId;
        m_panel->setBusy(true, -1);
        m_panel->setStatus(T("search.pulling").arg(variantId));
        // The tag size from the registry is the intended download total; the
        // parser uses it so the bar does not restart for every layer. Both the
        // registry figure and the CLI lines use decimal units.
        m_cli->pullModelAsync(variantId,
                              pullprogress::parseSizeBytes(m_panel->selectedVariantSizeText()));
        return;
    }

    m_pendingRepo = itemId;
    ensureHfToken();
    HfVariant chosen;
    bool found = false;
    for (const HfVariant &variant : std::as_const(m_hfVariants)) {
        if (variant.id == variantId) {
            chosen = variant;
            found = true;
            break;
        }
    }
    if (!found || chosen.files.isEmpty() || chosen.projector || chosen.draft)
        return;

    const QString dir = defaultDownloadDir();
    quint64 needed = chosen.size;
    // The repo's vision projector is downloaded together with the model.
    for (const HfVariant &other : std::as_const(m_hfVariants)) {
        if (other.projector) {
            needed += other.size;
            break;
        }
    }
    const quint64 required = requiredSpaceBytes(needed);
    const quint64 free = freeDiskBytes(dir);
    if (free > 0 && required > free) {
        m_panel->setStatus(T("browser.notEnoughSpace")
                           .arg(humanSize(qint64(required)), humanSize(qint64(free))));
        return;
    }

    startHfDownload(itemId, chosen);
}

void DiscoverPanel::startHfDownload(const QString &repo, const HfVariant &variant)
{
    m_pendingRepo = repo;
    m_pendingFile = variant.id;
    m_queueFirstFile = variant.id;
    m_queueFiles = variant.files;
    m_queueSizes = variant.sizes;
    while (m_queueSizes.size() < m_queueFiles.size())
        m_queueSizes.append(0);
    m_queueMainCount = m_queueFiles.size();
    m_queueIndex = 0;
    m_queueDoneBytes = 0;
    m_queueTotalBytes = qMax<qint64>(variant.size, 0);

    // A vision projector is a separate mmproj GGUF. Ollama attaches it through a
    // second FROM line, and without it the imported model rejects images with
    // "image input is not supported". Pull the first projector of the repo too.
    for (const HfVariant &other : std::as_const(m_hfVariants)) {
        if (!other.projector || other.files.isEmpty())
            continue;
        for (int i = 0; i < other.files.size(); ++i) {
            m_queueFiles.append(other.files.at(i));
            m_queueSizes.append(other.sizes.value(i));
        }
        m_queueTotalBytes += qMax<qint64>(other.size, 0);
        break;
    }

    m_queuePartTotal = 0;
    m_queueLastPath.clear();
    m_queueActive = true;
    m_cancelRequested = false;
    m_panel->setBusy(true, -1);
    startNextPart();
}

void DiscoverPanel::startNextPart()
{
    if (!m_queueActive)
        return;
    if (m_cancelRequested || m_queueIndex >= m_queueFiles.size()) {
        if (m_cancelRequested)
            stopHfQueue();
        else
            finishHfDownload();
        return;
    }

    // A retry after a failed part should not fetch the shards that already
    // completed: a file with the expected size counts as done (there is no
    // HTTP range resume, so a truncated file is simply fetched again).
    while (m_queueIndex < m_queueFiles.size()) {
        const QString localPath = QDir(defaultDownloadDir())
                .filePath(QFileInfo(m_queueFiles.at(m_queueIndex)).fileName());
        const qint64 expected = m_queueSizes.value(m_queueIndex);
        const QFileInfo info(localPath);
        if (expected <= 0 || !info.exists() || info.size() != expected)
            break;
        m_queueDoneBytes += expected;
        ++m_queueIndex;
    }
    if (m_queueIndex >= m_queueFiles.size()) {
        finishHfDownload();
        return;
    }

    const int total = m_queueFiles.size();
    const int percent = m_queueTotalBytes > 0
            ? int(m_queueDoneBytes * 100 / m_queueTotalBytes) : -1;
    m_panel->setBusy(true, qBound(-1, percent, 100));
    m_queuePartTotal = 0;
    if (total > 1) {
        m_panel->setStatus(T("browser.partProgress")
                           .arg(m_queueIndex + 1).arg(total)
                           + QStringLiteral(" · ")
                           + T("browser.progress")
                                     .arg(humanSize(m_queueDoneBytes),
                                          humanSize(m_queueTotalBytes)));
    } else {
        m_panel->setStatus(T("browser.downloading").arg(m_queueFiles.at(m_queueIndex)));
    }
    m_hf->download(m_pendingRepo, m_queueFiles.at(m_queueIndex), defaultDownloadDir());
}

void DiscoverPanel::stopHfQueue()
{
    m_queueActive = false;
    m_cancelRequested = false;
    m_queueFiles.clear();
    m_queueSizes.clear();
    m_panel->setBusy(false, 0);
}

void DiscoverPanel::finishHfDownload()
{
    const QString name = HfClient::modelNameFor(m_pendingRepo, m_queueFirstFile);
    const QString dir = defaultDownloadDir();
    const bool split = m_queueMainCount > 1;
    // Always derive the main file from the first shard: the last downloaded
    // file may already be the projector.
    QString from = split ? splitWildcardPattern(m_queueFirstFile)
                         : QFileInfo(m_queueFirstFile).fileName();
    from = QDir(dir).filePath(from);

    QString content = QStringLiteral("FROM ") + from + QLatin1Char('\n');
    if (m_queueFiles.size() > m_queueMainCount) {
        const QString projector = m_queueFiles.at(m_queueMainCount);
        const QString projectorFrom = QDir(dir).filePath(splitWildcardPattern(projector));
        content += QStringLiteral("FROM ") + projectorFrom + QLatin1Char('\n');
    }
    // Remember every downloaded shard (main + projector) before the queue is
    // cleared: they are deleted once `ollama create` has copied them.
    m_pendingCreateSources.clear();
    for (const QString &file : std::as_const(m_queueFiles)) {
        m_pendingCreateSources << QDir(dir).filePath(QFileInfo(file).fileName());
    }
    stopHfQueue();

    // The GGUF is on disk, but Ollama cannot use it until `ollama create`
    // imports it (a 100 GB split takes minutes). Keep the "on device" mark off
    // until that finishes; the CLI signal does the rest.
    m_pendingCreateName = name;
    m_pendingCreateRepo = m_pendingRepo;
    m_pendingCreateFile = m_queueFirstFile;

    // `ollama create` on a 100 GB model takes minutes: show a running bar and
    // lock the picker, but hide "Cancel" — the import cannot be interrupted.
    m_panel->setBusy(true, -1, false);
    m_panel->setStatus(T("browser.creating").arg(name));
    emit modelCreateRequested(name, content);
}

void DiscoverPanel::applyHfVariants(const QString &itemId)
{
    // m_hfVariants holds the file list of m_pendingRepo only; a stale call (the
    // user moved to another card while `ollama create` ran) must not push those
    // files into a different item.
    if (itemId != m_pendingRepo)
        return;
    QList<BrowserVariant> variants;
    variants.reserve(m_hfVariants.size());
    const QString sha = m_repoShas.value(itemId);
    for (const HfVariant &group : std::as_const(m_hfVariants)) {
        BrowserVariant variant;
        variant.id = group.id;
        variant.label = group.label;
        variant.size = humanSize(group.size);
        variant.sizeBytes = quint64(qMax<qint64>(group.size, 0));
        variant.fit = memoryinfo::fitLevel(variant.sizeBytes);
        if (group.draft) {
            variant.enabled = false;
            variant.hint = T("browser.draft");
        } else if (group.projector) {
            variant.enabled = false;
            variant.hint = T("browser.projector");
        } else if (group.split) {
            variant.enabled = true;
            variant.hint = T("browser.split").arg(group.parts);
        }
        // The picker appends the hint itself for disabled entries.
        const QString localName = HfClient::modelNameFor(itemId, group.id);
        QString installedSha;
        variant.installed = hfVariantInstalled(itemId, group.id, localName, &installedSha);
        variant.updateAvailable = variant.installed && shaDiffers(installedSha, sha);
        variants.append(variant);
    }
    m_panel->setVariants(itemId, variants);
    m_panel->setStatus(variants.isEmpty() ? T("browser.noVariants")
                                          : T("search.variantsFound").arg(variants.size()));
}

void DiscoverPanel::applyOllamaState(const QString &itemId, const QString &ref)
{
    const bool installed = m_installedNames.contains(ref);
    bool update = false;
    const QString digest = m_manifestDigests.value(ref);
    if (installed && !digest.isEmpty()) {
        // Same rule the automatic source search uses: a short id from
        // `ollama list` matches its full registry digest by prefix.
        update = !digestMatches(m_localIds.value(ref), digest);
    }
    m_panel->setVariantState(itemId, ref, installed, update);
}

void DiscoverPanel::checkOllamaUpdates(const QString &itemId, const QStringList &refs)
{
    // RegistryClient holds one manifest reply, so checks are strictly
    // sequential. Refs answered earlier in the session are re-applied from the
    // cache; only the unknown ones go on the wire.
    m_updateQueue.clear();
    m_updateItemId = itemId;
    for (const QString &ref : refs) {
        if (m_manifestDigests.contains(ref))
            applyOllamaState(itemId, ref);
        else
            m_updateQueue.append(ref);
    }
    startNextUpdateCheck();
}

void DiscoverPanel::startNextUpdateCheck()
{
    if (m_updateQueue.isEmpty())
        return;
    m_registry->checkManifest(m_updateQueue.first());
}

void DiscoverPanel::applyHfStates(const QString &repo)
{
    // m_hfVariants belongs to the repository that was opened last; a late reply
    // for another one is only cached, its marks are applied on the next visit.
    if (repo != m_pendingRepo)
        return;
    const QString sha = m_repoShas.value(repo);
    for (const HfVariant &group : std::as_const(m_hfVariants)) {
        const QString localName = HfClient::modelNameFor(repo, group.id);
        QString installedSha;
        const bool installed = hfVariantInstalled(repo, group.id, localName, &installedSha);
        m_panel->setVariantState(repo, group.id, installed,
                                 installed && shaDiffers(installedSha, sha));
    }
}

void DiscoverPanel::ensureRepoInfo(const QString &repo)
{
    if (repo.isEmpty())
        return;
    if (m_repoShas.contains(repo)) {
        applyHfStates(repo); // already known this session
        return;
    }
    if (m_repoInfoRequested.contains(repo))
        return; // a request is already in flight
    m_repoInfoRequested.insert(repo);
    ensureHfToken();
    m_hf->fetchRepoInfo(repo);
}

bool DiscoverPanel::hfVariantInstalled(const QString &repo, const QString &firstFile,
                                       const QString &localName,
                                       QString *installedSha) const
{
    // Provenance first: it says which repository file each installed model came
    // from, which survives a rename and an older naming scheme. Two quants of
    // one repo (`IQ4_XS` and `IQ4_XS-MTP`) must not mark each other.
    for (const QString &name : std::as_const(m_installedNames)) {
        const Provenance::Entry entry = Provenance::instance().lookup(name);
        if (entry.source != QLatin1String("hf") || entry.ref != repo)
            continue;
        if (entry.file.isEmpty() || !sameGgufVariant(entry.file, firstFile))
            continue;
        if (installedSha)
            *installedSha = entry.sha;
        return true;
    }

    // Fallback for models installed before provenance was tracked. It must stay
    // quiet when provenance *does* know the name: that entry points at another
    // file (the legacy `:iq4_xs` model was built from the MTP parts), and the
    // name alone would mark the wrong row.
    if (m_installedNames.contains(localName)
            && Provenance::instance().lookup(localName).isEmpty()) {
        if (installedSha)
            installedSha->clear();
        return true;
    }
    return false;
}

void DiscoverPanel::onCancel()
{
    if (isOllama()) {
        if (m_cli->isPulling())
            m_cli->cancelPull();
    } else {
        if (m_queueActive) {
            // Abort the part in flight; the queue stops when the reply reports
            // the cancellation instead of moving on to the next shard.
            m_cancelRequested = true;
        }
        m_hf->cancelDownload();
    }
}

void DiscoverPanel::onPullProgress(const QString &label, int percent, qint64 received,
                                   qint64 total)
{
    m_panel->setBusy(true, percent < 0 ? -1 : percent);
    if (total > 0) {
        m_panel->setStatus(T("browser.progress")
                           .arg(humanSize(received), humanSize(total)));
    } else if (received > 0) {
        m_panel->setStatus(T("browser.progressNoTotal").arg(humanSize(received)));
    } else if (!label.isEmpty()) {
        // Nothing counted yet: show what the CLI is doing ("pulling manifest").
        m_panel->setStatus(label);
    }
}

void DiscoverPanel::onPullFinished(bool ok, const QString &output)
{
    m_panel->setBusy(false, 0);
    if (!ok) {
        m_panel->setStatus(T("search.installFailed").arg(output));
        return;
    }

    const QString pulled = m_pullingName;
    Provenance::Entry entry;
    entry.source = QStringLiteral("ollama");
    entry.ref = m_pullingName;
    Provenance::instance().record(m_pullingName, entry);

    m_panel->setStatus(T("search.installed").arg(m_pullingName));
    m_pullingName.clear();
    refreshInstalled();
    // The pull just fetched the manifest the registry serves now, so the
    // variant flips to "скачано" (and an "обновить" mark disappears) without
    // reloading the tab.
    m_panel->setVariantState(m_pendingItemId, pulled, true, false);
    emit installedChanged();
}

void DiscoverPanel::onDownloadProgress(qint64 received, qint64 total)
{
    if (!m_queueActive)
        return;
    if (total > 0)
        m_queuePartTotal = total;

    const qint64 receivedTotal = m_queueDoneBytes + received;
    const qint64 planned = m_queueTotalBytes > 0 ? m_queueTotalBytes
                                                 : m_queueDoneBytes + m_queuePartTotal;
    const int percent = planned > 0
            ? qBound(0, int(receivedTotal * 100 / planned), 100) : -1;
    m_panel->setBusy(true, percent);

    QString status = planned > 0
            ? T("browser.progress").arg(humanSize(receivedTotal), humanSize(planned))
            : T("browser.progressNoTotal").arg(humanSize(receivedTotal));
    if (m_queueFiles.size() > 1) {
        status = T("browser.partProgress")
                         .arg(m_queueIndex + 1).arg(m_queueFiles.size())
                + QStringLiteral(" · ") + status;
    }
    m_panel->setStatus(status);
}

void DiscoverPanel::onDownloadFinished(bool ok, const QString &path, const QString &error)
{
    if (!m_queueActive) {
        // A stale reply from a download that was replaced by this queue.
        m_panel->setBusy(false, 0);
        return;
    }

    const bool cancelled = m_cancelRequested
            || error == QLatin1String("cancelled");
    if (!ok) {
        const int part = m_queueIndex + 1;
        const int parts = m_queueFiles.size();
        // Keep the shards that already finished: a retry only has to fetch the
        // remaining parts, and the finished files are what the Modelfile needs.
        stopHfQueue();
        if (cancelled)
            m_panel->setStatus(T("browser.downloadCancelled"));
        else if (parts > 1)
            m_panel->setStatus(T("browser.downloadFailedPart")
                               .arg(part).arg(parts).arg(hfErrorText(error)));
        else
            m_panel->setStatus(T("browser.downloadFailed").arg(hfErrorText(error)));
        return;
    }

    m_queueLastPath = path;
    const qint64 partBytes = m_queueSizes.value(m_queueIndex);
    m_queueDoneBytes += partBytes > 0 ? partBytes : m_queuePartTotal;
    ++m_queueIndex;
    m_queuePartTotal = 0;

    if (m_cancelRequested) {
        stopHfQueue();
        m_panel->setStatus(T("browser.downloadCancelled"));
        return;
    }
    startNextPart();
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
    title->setProperty("section", true);
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
    sizeTitle->setProperty("section", true);
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
        rebuildItems();
    });
    layout->addWidget(sizeCombo);

    auto *uploaderTitle = new QLabel(T("filters.uploader"));
    uploaderTitle->setProperty("section", true);
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
