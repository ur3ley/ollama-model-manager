#include "discoverpanel.h"
#include "apppaths.h"

#include "designtokens.h"
#include "format.h"
#include "combohighlightdelegate.h"
#include "hfauth.h"
#include "hftokendialog.h"
#include "ggufmeta.h"
#include "hfvariants.h"
#include "i18n.h"
#include "installservice.h"
#include "memoryinfo.h"
#include "modelbrowserpanel.h"
#include "modelfilebuilder.h"
#include "ollamacli.h"
#include "popupcombobox.h"
#include "provenance.h"
#include "pullprogress.h"
#include "quantmatch.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QTimer>
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

// Sizes in this panel show "—" when unknown.
QString humanSize(qint64 bytes)
{
    return format::byteSize(bytes, QStringLiteral("—"));
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

// Shared with MainWindow's source cleanup: both must look in the same place.
QString defaultDownloadDir()
{
    return apppaths::downloadDir();
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

DiscoverPanel::DiscoverPanel(Source source, InstallService *install, QWidget *parent)
    : QWidget(parent)
    , m_source(source)
    , m_install(install)
    , m_cli(install->cli())
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

    // Installed marks come from the list the main window refreshes anyway;
    // asking `ollama list` synchronously on every card click froze the UI.
    connect(m_cli, &OllamaCli::modelsListed, this,
            [this](bool ok, const QList<OllamaCli::ModelInfo> &models, const QString &) {
        if (ok)
            applyInstalledList(models);
    });
    // Queue changes from the global downloads panel: the running pull may be
    // pushed back and another one promoted. Keep the per-variant badges and
    // this panel's bookkeeping in sync with the job that really runs.
    connect(m_cli, &OllamaCli::pullSuspended, this, [this](quint64 id) {
        if (!m_pullJob || m_pullJob->id() != id)
            return;
        m_queuedPulls.prepend(QueuedPull{m_pullJob, m_pullItemId, m_pullVariantId});
        m_panel->setVariantJobState(m_pullItemId, m_pullVariantId,
                                    ModelBrowserPanel::JobState::Queued);
        m_pullJob.clear();
        m_pullItemId.clear();
        m_pullVariantId.clear();
    });
    connect(m_cli, &OllamaCli::pullStarted, this, [this](quint64 id) {
        for (int i = 0; i < m_queuedPulls.size(); ++i) {
            const QueuedPull entry = m_queuedPulls.at(i);
            if (!entry.job || entry.job->id() != id)
                continue;
            m_queuedPulls.removeAt(i);
            m_pullJob = entry.job;
            m_pullItemId = entry.itemId;
            m_pullVariantId = entry.variantId;
            m_panel->setVariantJobState(entry.itemId, entry.variantId,
                                        ModelBrowserPanel::JobState::Running);
            m_panel->setStatus(T("search.pulling").arg(entry.variantId));
            return;
        }
    });
    connect(m_hf, &HfClient::downloadProgress, this, &DiscoverPanel::onDownloadProgress);
    connect(m_hf, &HfClient::downloadFinished, this, &DiscoverPanel::onDownloadFinished);

    if (!isOllama()) {
        loadHfQueue();
        // The main window connects to hfActivityChanged after building the
        // panels; report the restored queue once the event loop runs.
        QTimer::singleShot(0, this, [this]() { notifyHfActivity(); });
    }
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
    connect(m_panel, &ModelBrowserPanel::cancelVariantRequested,
            this, &DiscoverPanel::onCancelVariant);

    updateFilterButton();
}

void DiscoverPanel::activate()
{
    if (m_loaded)
        return;
    m_loaded = true;
    if (isOllama())
        m_cli->resumeSavedQueue();
    if (!isOllama() && m_hfResumePending && !m_queueActive && !m_createJob) {
        // The queue survived a restart: continue where it stopped.
        m_hfResumePending = false;
        if (!m_hfQueue.isEmpty()) {
            const HfPending next = m_hfQueue.takeFirst();
            m_pendingItemId = next.itemId;
            saveHfQueue();
            startHfDownload(next.repo, next.variant);
        }
    }
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
        for (const RegistryClient::Model &model : std::as_const(sorted)) {
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
        for (const HfClient::Repo &repo : std::as_const(m_hfResults)) {
            const QStringList caps = filterlogic::hfTagsToCapabilities(repo.tags);
            if (!filterlogic::hasCapabilities(caps, m_filters.capabilities))
                continue;
            if (!uploader.isEmpty() && !repo.id.contains(uploader, Qt::CaseInsensitive))
                continue;

            BrowserItem item;
            item.id = repo.id;
            const int slash = repo.id.indexOf(QLatin1Char('/'));
            item.title = slash >= 0 ? repo.id.mid(slash + 1) : repo.id;
            item.subtitle = slash >= 0 ? repo.id.left(slash) : QString();
            item.badges = QStringList{QStringLiteral("gguf")} + caps;
            item.stats = T("browser.stats")
                    .arg(format::compactCount(quint64(qMax<qint64>(repo.downloads, 0))), format::compactCount(quint64(qMax<qint64>(repo.likes, 0))));
            items.append(item);
        }
    }

    m_panel->setItems(items);
    m_panel->setStatus(items.isEmpty() ? T("search.nothingFound")
                                       : T("search.modelsFound").arg(items.size()));
}

void DiscoverPanel::applyInstalledList(const QList<OllamaCli::ModelInfo> &models)
{
    m_installedNames.clear();
    m_localIds.clear();
    for (const OllamaCli::ModelInfo &model : models) {
        m_installedNames.insert(model.name);
        // The id is the manifest digest `ollama list` printed: the local side
        // of the update comparison.
        m_localIds.insert(model.name, model.id);
    }
}

void DiscoverPanel::onItemSelected(const QString &itemId)
{
    m_pendingItemId = itemId;
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
        if (variantId.isEmpty())
            return;
        if (m_pullVariants.contains(variantId)) {
            // Same variant again: the running/queued job already covers it.
            m_panel->setStatus(T("search.pullQueued").arg(variantId));
            return;
        }
        const bool hasOwn = m_pullJob || !m_queuedPulls.isEmpty();
        // Sample before creating the job: after that, our own pull counts too.
        const bool otherPulling = m_cli->isPulling();
        if (!hasOwn) {
            // Registry pulls are queueable: the list stays usable, so the next
            // click adds a job instead of being swallowed by a locked tab.
            m_panel->setBusy(true, -1, true, /*queueable=*/true);
        }
        // Another tab may be pulling: ours waits in the CLI queue instead of
        // killing that download (B-2).
        m_panel->setStatus(hasOwn || otherPulling ? T("search.pullQueued").arg(variantId)
                                                  : T("search.pulling").arg(variantId));
        // The tag size from the registry is the intended download total; the
        // parser uses it so the bar does not restart for every layer. Both the
        // registry figure and the CLI lines use decimal units.
        OllamaJob *job = m_install->pullFromRegistry(
                variantId, pullprogress::parseSizeBytes(m_panel->selectedVariantSizeText()));
        m_pullVariants.insert(variantId);
        if (!hasOwn) {
            m_pullJob = job;
            m_pullItemId = itemId;
            m_pullVariantId = variantId;
            m_panel->setVariantJobState(itemId, variantId,
                                        otherPulling ? ModelBrowserPanel::JobState::Queued
                                                     : ModelBrowserPanel::JobState::Running);
        } else {
            m_queuedPulls.append(QueuedPull{job, itemId, variantId});
            m_panel->setVariantJobState(itemId, variantId, ModelBrowserPanel::JobState::Queued);
        }
        connect(job, &OllamaJob::progress, this,
                [this, job](const QString &label, int percent, qint64 received, qint64 total) {
            // Queued jobs start only after the running one exits; until then
            // their progress would fight over the single bar.
            if (job != m_pullJob)
                return;
            // Ours may have waited behind another tab's pull: it runs now.
            m_panel->setVariantJobState(m_pullItemId, m_pullVariantId,
                                        ModelBrowserPanel::JobState::Running);
            onPullProgress(label, percent, received, total);
        });
        connect(job, &OllamaJob::finished, this, [this, job, itemId, variantId](bool ok, const QString &output) {
            onPullFinished(job, itemId, variantId, ok, output);
        });
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
    // One HuggingFace import at a time: a second variant waits in this panel's
    // queue instead of replacing the running download.
    if (m_queueActive || m_createJob) {
        m_hfQueue.append(HfPending{repo, m_pendingItemId, variant});
        m_panel->setVariantJobState(m_pendingItemId, variant.id,
                                    ModelBrowserPanel::JobState::Queued);
        m_panel->setStatus(T("search.pullQueued").arg(
                variant.label.isEmpty() ? variant.id : variant.label));
        saveHfQueue();
        notifyHfActivity();
        return;
    }
    m_activeHfItemId = m_pendingItemId;
    m_activeHfVariantId = variant.id;
    m_activeHfVariant = variant;
    m_panel->setVariantJobState(m_activeHfItemId, m_activeHfVariantId,
                                ModelBrowserPanel::JobState::Running);

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
    // Every shard from the same commit, and provenance records exactly that
    // commit (empty when the repo info has not arrived: `main`, as before).
    m_queueRevision = m_repoShas.value(repo);
    // Keep enough metadata next to the partial files to offer "Продолжить"
    // after a crash or a lost network, not just while the app runs.
    writeHfSidecar(QDir(defaultDownloadDir()).filePath(
            QFileInfo(variant.id).fileName() + QStringLiteral(".omm.json")), repo, variant);
    m_queueActive = true;
    m_cancelRequested = false;
    m_panel->setBusy(true, -1, true, /*queueable=*/true);
    saveHfQueue();
    notifyHfActivity();
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
    // completed: a file with the expected size counts as done (an unfinished
    // shard stays a `.part`, which HfClient resumes with an HTTP range).
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
    m_panel->setBusy(true, qBound(-1, percent, 100), true, /*queueable=*/true);
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
    m_hf->download(m_pendingRepo, m_queueFiles.at(m_queueIndex), defaultDownloadDir(),
                   m_queueSizes.value(m_queueIndex), m_queueRevision);
}

void DiscoverPanel::stopHfQueue()
{
    m_queueActive = false;
    m_cancelRequested = false;
    m_queueFiles.clear();
    m_queueSizes.clear();
    m_panel->setBusy(false, 0);
    saveHfQueue();
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

    // Quoted when the path has spaces (ModelfileBuilder::fromLine).
    QString content = ModelfileBuilder::fromLine(from);
    if (m_queueFiles.size() > m_queueMainCount) {
        const QString projector = m_queueFiles.at(m_queueMainCount);
        const QString projectorFrom = QDir(dir).filePath(splitWildcardPattern(projector));
        content += ModelfileBuilder::fromLine(projectorFrom);
    }
    // Sampling hints: GGUF may carry llama.cpp's recommended values under
    // `general.sampling.*`; otherwise use Ollama's own defaults, so clients
    // that cannot pass `options` still get sane numbers instead of zeros.
    double samplingTemp = 0.80;
    double samplingTopP = 0.90;
    int samplingTopK = 40;
    const QString firstFile = QDir(dir).filePath(QFileInfo(m_queueFirstFile).fileName());
    const GgufMeta gguf = GgufMeta::read(firstFile);
    if (gguf.ok) {
        if (gguf.samplingTemperature > 0)
            samplingTemp = gguf.samplingTemperature;
        if (gguf.samplingTopP > 0)
            samplingTopP = gguf.samplingTopP;
        if (gguf.samplingTopK > 0)
            samplingTopK = gguf.samplingTopK;
    }
    content += QStringLiteral("PARAMETER temperature ")
            + QString::number(samplingTemp, 'f', 2) + QLatin1Char('\n');
    content += QStringLiteral("PARAMETER top_p ")
            + QString::number(samplingTopP, 'f', 2) + QLatin1Char('\n');
    content += QStringLiteral("PARAMETER top_k ")
            + QString::number(samplingTopK) + QLatin1Char('\n');
    InstallService::HfImport request;
    request.name = name;
    request.modelfile = content;
    request.repo = m_pendingRepo;
    request.file = m_queueFirstFile;
    request.sha = m_queueRevision.isEmpty() ? m_repoShas.value(m_pendingRepo) : m_queueRevision;
    // Every downloaded shard (main + projector), taken before the queue is
    // cleared: they are deleted once `ollama create` has copied them.
    for (const QString &file : std::as_const(m_queueFiles))
        request.sources << QDir(dir).filePath(QFileInfo(file).fileName());
    stopHfQueue();
    QFile::remove(QDir(defaultDownloadDir()).filePath(
            QFileInfo(m_queueFirstFile).fileName() + QStringLiteral(".omm.json")));

    // The GGUF is on disk, but Ollama cannot use it until `ollama create`
    // imports it (a 100 GB split takes minutes). Keep the "on device" mark off
    // until our job finishes — an editor update finishing first is not ours.
    // `ollama create` on a 100 GB model takes minutes: show a running bar and
    // lock the picker, but hide "Cancel" — the import cannot be interrupted.
    // The create step cannot be cancelled, but the list stays usable so more
    // imports can be queued behind it.
    m_hfActiveSources = request.sources;
    m_panel->setBusy(true, -1, false, /*queueable=*/true);
    m_panel->setStatus(T("browser.creating").arg(name));
    OllamaJob *job = m_install->importHf(request);
    m_createJob = job;
    // The download part ended, but the model is not usable until create
    // finishes: keep the global counter at one for this import.
    notifyHfActivity();
    connect(job, &OllamaJob::finished, this, [this, job, name](bool ok, const QString &output) {
        onCreateFinished(name, ok, output,
                         job->property(InstallService::kFreedBytes).toULongLong());
    });
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
        const Provenance::Entry entry = m_install->provenance()->lookup(name);
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
            && m_install->provenance()->lookup(localName).isEmpty()) {
        if (installedSha)
            installedSha->clear();
        return true;
    }
    return false;
}

void DiscoverPanel::onCancelVariant(const QString &itemId, const QString &variantId)
{
    Q_UNUSED(itemId);
    if (!isOllama()) {
        cancelHfTask(variantId);
        return;
    }
    // The action button doubles as "Отмена" for this variant: cancel exactly
    // the job behind it, not the whole queue.
    if (m_pullJob && m_pullVariantId == variantId) {
        m_pullJob->cancel();
        return;
    }
    for (int i = 0; i < m_queuedPulls.size(); ++i) {
        if (m_queuedPulls.at(i).variantId != variantId)
            continue;
        const QueuedPull entry = m_queuedPulls.takeAt(i);
        m_pullVariants.remove(entry.variantId);
        m_panel->setVariantJobState(entry.itemId, entry.variantId,
                                    ModelBrowserPanel::JobState::None);
        if (entry.job)
            entry.job->cancel();
        if (!m_pullJob && m_queuedPulls.isEmpty())
            m_panel->setBusy(false, 0);
        return;
    }
}

void DiscoverPanel::onCancel()
{
    if (isOllama()) {
        // Only our own pulls: the editor's download is not this tab's to stop.
        // Clear the waiting ones first, so a promoted job cannot slip out of
        // the queue while the running one reports its cancellation.
        const auto queued = m_queuedPulls;
        m_queuedPulls.clear();
        for (const QueuedPull &entry : queued) {
            m_pullVariants.remove(entry.variantId);
            m_panel->setVariantJobState(entry.itemId, entry.variantId,
                                        ModelBrowserPanel::JobState::None);
            if (entry.job)
                entry.job->cancel();
        }
        if (m_pullJob) {
            m_pullVariants.remove(m_pullVariantId);
            m_panel->setVariantJobState(m_pullItemId, m_pullVariantId,
                                        ModelBrowserPanel::JobState::None);
            m_pullJob->cancel();
        }
    } else {
        if (m_queueActive) {
            // Abort the part in flight; the queue stops when the reply reports
            // the cancellation instead of moving on to the next shard.
            m_cancelRequested = true;
        }
        m_hf->cancelDownload();
    }
}

void DiscoverPanel::onPullProgress(const QString &label, int percent,
                                   qint64 received, qint64 total)
{
    // Keep the tab queueable: this fires on every progress tick of a pull, and
    // dropping the flag here would re-lock the list after the first update.
    m_panel->setBusy(true, percent < 0 ? -1 : percent, true, /*queueable=*/true);
    QString status;
    if (total > 0) {
        status = T("browser.progress").arg(humanSize(received), humanSize(total));
    } else if (received > 0) {
        status = T("browser.progressNoTotal").arg(humanSize(received));
    } else if (!label.isEmpty()) {
        // Nothing counted yet: show what the CLI is doing ("pulling manifest").
        status = label;
    }
    if (!m_queuedPulls.isEmpty()) {
        // The progress line is rewritten on every tick, so the queue count must
        // live inside it — otherwise a queued click has no lasting feedback.
        if (!status.isEmpty())
            status += QStringLiteral(" · ");
        status += T("search.queuedCount").arg(m_queuedPulls.size());
    }
    if (!status.isEmpty())
        m_panel->setStatus(status);
}

void DiscoverPanel::onPullFinished(OllamaJob *job, const QString &itemId, const QString &pulled,
                                   bool ok, const QString &output)
{
    // Called for this panel's own job only; InstallService has already
    // recorded the provenance.
    m_pullVariants.remove(pulled);
    m_panel->setVariantJobState(itemId, pulled, ModelBrowserPanel::JobState::None);
    if (job != m_pullJob) {
        // A suspended pull that finished later (it was promoted by the CLI on
        // its own turn): drop it from our waiting list, but keep its result.
        for (int i = 0; i < m_queuedPulls.size(); ++i) {
            if (m_queuedPulls.at(i).job == job) {
                m_queuedPulls.removeAt(i);
                break;
            }
        }
        if (ok) {
            m_installedNames.insert(pulled);
            m_panel->setVariantState(itemId, pulled, true, false);
            emit installedChanged();
        }
        return;
    }

    m_pullJob.clear();
    m_pullItemId.clear();
    m_pullVariantId.clear();
    // Promote this tab's next queued pull; OllamaCli starts it as soon as the
    // previous `ollama pull` exits.
    while (!m_queuedPulls.isEmpty() && !m_queuedPulls.first().job)
        m_queuedPulls.removeFirst();
    if (!m_queuedPulls.isEmpty()) {
        const QueuedPull next = m_queuedPulls.takeFirst();
        m_pullJob = next.job;
        m_pullItemId = next.itemId;
        m_pullVariantId = next.variantId;
        m_panel->setVariantJobState(next.itemId, next.variantId,
                                    ModelBrowserPanel::JobState::Running);
        m_panel->setBusy(true, -1, true, /*queueable=*/true);
    } else {
        m_panel->setBusy(false, 0);
    }

    if (!ok) {
        if (output.compare(QLatin1String("cancelled"), Qt::CaseInsensitive) == 0)
            m_panel->setStatus(T("search.cancelled"));
        else
            m_panel->setStatus(T("search.installFailed").arg(output));
        return;
    }

    m_panel->setStatus(T("search.installed").arg(pulled));
    m_installedNames.insert(pulled);
    // The pull just fetched the manifest the registry serves now, so the
    // variant flips to "скачано" (and an "обновить" mark disappears) without
    // reloading the tab.
    m_panel->setVariantState(itemId, pulled, true, false);
    emit installedChanged();
}

void DiscoverPanel::onCreateFinished(const QString &name, bool ok, const QString &output,
                                     quint64 freed)
{
    // Our own import job; provenance and source cleanup are done already.
    m_createJob.clear();
    m_hfActiveSources.clear();
    m_panel->setBusy(false, 0);
    if (!ok) {
        m_panel->setStatus(T("browser.createFailed").arg(output));
        advanceHfQueue();
        return;
    }
    // The full list follows via installedChanged → refresh → modelsListed.
    m_installedNames.insert(name);
    if (!m_pendingItemId.isEmpty())
        applyHfVariants(m_pendingItemId);
    m_panel->setStatus(freed > 0
            ? T("browser.installedCleaned").arg(name, humanSize(qint64(freed)))
            : T("search.installed").arg(name));
    emit installedChanged();
    advanceHfQueue();
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
    // Keep the tab queueable: this fires on every progress tick, and dropping
    // the flag here would re-lock the repo list mid-download.
    m_panel->setBusy(true, percent, true, /*queueable=*/true);

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
    if (m_hfIgnoreCancel && error == QLatin1String("cancelled")) {
        // The stop we requested for a drag-and-drop promotion; the next task
        // was already started synchronously.
        m_hfIgnoreCancel = false;
        return;
    }
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
        // A user cancel, however, means "I do not want this model": drop them.
        if (cancelled)
            removeHfPartialSources();
        stopHfQueue();
        if (cancelled)
            m_panel->setStatus(T("browser.downloadCancelled"));
        else if (parts > 1)
            m_panel->setStatus(T("browser.downloadFailedPart")
                               .arg(part).arg(parts).arg(hfErrorText(error)));
        else
            m_panel->setStatus(T("browser.downloadFailed").arg(hfErrorText(error)));
        advanceHfQueue();
        return;
    }

    m_queueLastPath = path;
    const qint64 partBytes = m_queueSizes.value(m_queueIndex);
    m_queueDoneBytes += partBytes > 0 ? partBytes : m_queuePartTotal;
    ++m_queueIndex;
    m_queuePartTotal = 0;

    if (m_cancelRequested) {
        removeHfPartialSources();
        stopHfQueue();
        m_panel->setStatus(T("browser.downloadCancelled"));
        advanceHfQueue();
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
    // Diagnostic for the offscreen self-test's failure messages: what the panel
    // actually holds, in one line (it used to be a stub that returned "").
    QString text = QStringLiteral("source=%1 items=%2")
                       .arg(isOllama() ? QStringLiteral("ollama") : QStringLiteral("hf"))
                       .arg(m_panel ? m_panel->itemCount() : 0);
    if (m_pullJob)
        text += QStringLiteral(" pull=running");
    if (!m_queuedPulls.isEmpty())
        text += QStringLiteral(" pull=queued:%1").arg(m_queuedPulls.size());
    if (m_createJob)
        text += QStringLiteral(" create=%1").arg(m_createJob->name());
    if (m_queueActive)
        text += QStringLiteral(" download=%1/%2")
                        .arg(m_queueIndex)
                        .arg(m_queueFiles.size());
    return text;
}

void DiscoverPanel::startInstall()
{
    onAction(m_panel->selectedItemId(), m_panel->selectedVariantId());
}

QString DiscoverPanel::selectedVariantText() const
{
    return m_panel->selectedVariantText();
}

void DiscoverPanel::cancelSelectedVariant()
{
    onCancelVariant(m_panel->selectedItemId(), m_panel->selectedVariantId());
}

QString DiscoverPanel::hfActivityText() const
{
    if (isOllama())
        return QString();
    QString text;
    if (m_queueActive && m_queueIndex < m_queueFiles.size())
        text = QFileInfo(m_queueFiles.at(m_queueIndex)).fileName();
    else if (m_createJob)
        text = m_createJob->name();
    else if (!m_hfQueue.isEmpty())
        text = m_hfQueue.first().variant.label;
    if (!m_hfQueue.isEmpty() && (m_queueActive || m_createJob))
        text += T("downloads.plusQueued").arg(m_hfQueue.size());
    return text;
}

bool DiscoverPanel::hfCancellable() const
{
    // `ollama create` cannot be interrupted; the download part can.
    return !isOllama() && m_queueActive && !m_createJob;
}

QList<DiscoverPanel::HfTaskInfo> DiscoverPanel::hfTasks() const
{
    QList<HfTaskInfo> tasks;
    if (isOllama())
        return tasks;
    if (m_queueActive || m_createJob) {
        HfTaskInfo task;
        task.id = m_activeHfVariantId;
        if (m_queueActive && m_queueIndex < m_queueFiles.size())
            task.label = QFileInfo(m_queueFiles.at(m_queueIndex)).fileName();
        else if (m_createJob)
            task.label = m_createJob->name();
        task.active = true;
        task.creating = m_createJob != nullptr;
        task.cancellable = m_queueActive && !m_createJob;
        tasks.append(task);
    }
    for (const HfPending &pending : m_hfQueue) {
        HfTaskInfo task;
        task.id = pending.variant.id;
        task.label = pending.variant.label.isEmpty() ? pending.variant.id
                                                     : pending.variant.label;
        task.cancellable = true;
        tasks.append(task);
    }
    return tasks;
}

QString DiscoverPanel::hfQueuePath() const
{
    return QDir(apppaths::configDir()).filePath(QStringLiteral("hf-queue.json"));
}

void DiscoverPanel::saveHfQueue() const
{
    QJsonArray tasks;
    auto append = [&tasks](const QString &repo, const QString &itemId, const HfVariant &variant) {
        QJsonObject task;
        task.insert(QStringLiteral("repo"), repo);
        task.insert(QStringLiteral("itemId"), itemId);
        task.insert(QStringLiteral("id"), variant.id);
        task.insert(QStringLiteral("label"), variant.label);
        task.insert(QStringLiteral("size"), double(variant.size));
        QJsonArray files;
        QJsonArray sizes;
        for (int i = 0; i < variant.files.size(); ++i) {
            files.append(variant.files.at(i));
            sizes.append(double(variant.sizes.value(i)));
        }
        task.insert(QStringLiteral("files"), files);
        task.insert(QStringLiteral("sizes"), sizes);
        tasks.append(task);
    };
    if (m_queueActive && !m_activeHfVariant.id.isEmpty())
        append(m_pendingRepo, m_activeHfItemId, m_activeHfVariant);
    for (const HfPending &pending : std::as_const(m_hfQueue))
        append(pending.repo, pending.itemId, pending.variant);

    const QString path = hfQueuePath();
    if (tasks.isEmpty()) {
        QFile::remove(path);
        return;
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("tasks"), tasks);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void DiscoverPanel::loadHfQueue()
{
    QFile file(hfQueuePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonArray tasks = QJsonDocument::fromJson(file.readAll())
            .object().value(QStringLiteral("tasks")).toArray();
    for (const QJsonValue &value : tasks) {
        const QJsonObject task = value.toObject();
        HfPending pending;
        pending.repo = task.value(QStringLiteral("repo")).toString();
        pending.itemId = task.value(QStringLiteral("itemId")).toString();
        pending.variant.id = task.value(QStringLiteral("id")).toString();
        pending.variant.label = task.value(QStringLiteral("label")).toString();
        pending.variant.size = qint64(task.value(QStringLiteral("size")).toDouble());
        const QJsonArray files = task.value(QStringLiteral("files")).toArray();
        const QJsonArray sizes = task.value(QStringLiteral("sizes")).toArray();
        for (int i = 0; i < files.size(); ++i) {
            pending.variant.files.append(files.at(i).toString());
            pending.variant.sizes.append(qint64(sizes.at(i).toDouble()));
        }
        if (pending.variant.id.isEmpty())
            continue;
        m_hfQueue.append(pending);
    }
    m_hfResumePending = !m_hfQueue.isEmpty();
}

QList<DiscoverPanel::HfIncomplete> DiscoverPanel::hfIncomplete() const
{
    QList<HfIncomplete> out;
    if (isOllama())
        return out;
    const QDir dir(defaultDownloadDir());
    QSet<QString> coveredFiles;  // every file named by any sidecar
    const QStringList names = dir.entryList({QStringLiteral("*.omm.json")}, QDir::Files);
    for (const QString &name : names) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || id == m_activeHfVariantId)
            continue;
        bool queued = false;
        for (const HfPending &pending : m_hfQueue)
            if (pending.variant.id == id)
                queued = true;
        if (queued)
            continue;

        HfIncomplete item;
        item.sidecar = file.fileName();
        item.total = qint64(object.value(QStringLiteral("total")).toDouble());
        item.label = object.value(QStringLiteral("label")).toString();
        const QJsonArray files = object.value(QStringLiteral("files")).toArray();
        for (const QJsonValue &value : files)
            coveredFiles.insert(QFileInfo(value.toString()).fileName());
        if (item.label.isEmpty() && !files.isEmpty())
            item.label = QFileInfo(files.first().toString()).fileName();

        // A queued/suspended import must read "в очереди", never "Продолжить":
        // match by id, first file name and visible label (old sidecars may
        // carry a variant id that differs from what the queue holds).
        QSet<QString> owned;
        auto addOwned = [&owned](const HfVariant &variant) {
            if (!variant.id.isEmpty()) {
                owned.insert(variant.id);
                owned.insert(QFileInfo(variant.id).fileName());
            }
            if (!variant.label.isEmpty())
                owned.insert(variant.label);
            for (const QString &source : variant.files)
                owned.insert(QFileInfo(source).fileName());
        };
        addOwned(m_activeHfVariant);
        for (const HfPending &pending : std::as_const(m_hfQueue))
            addOwned(pending.variant);
        if (owned.contains(id))
            continue;
        if (!files.isEmpty()) {
            const QString first = QFileInfo(files.first().toString()).fileName();
            if (owned.contains(first) || owned.contains(item.label))
                continue;
        }
        for (const QJsonValue &value : files) {
            const QString base = dir.filePath(QFileInfo(value.toString()).fileName());
            const QFileInfo whole(base);
            if (whole.exists()) {
                item.downloaded += whole.size();
                continue;
            }
            const QFileInfo part(base + QStringLiteral(".part"));
            if (part.exists())
                item.downloaded += part.size();
        }
        // Nothing downloaded → a queued task that never started; everything
        // downloaded → the file is whole (the import may still be in create).
        // Neither belongs in "Incomplete".
        if (item.downloaded <= 0)
            continue;
        if (item.total > 0 && item.downloaded >= item.total)
            continue;
        out.append(item);
    }

    // Partials left by older versions have no sidecar: list them too, so the
    // space is visible and can be reclaimed (no resume metadata for them).
    const QStringList parts = dir.entryList({QStringLiteral("*.part")}, QDir::Files);
    for (const QString &name : parts) {
        const QString base = name.left(name.size() - 5);  // strip ".part"
        // Vision projectors are part of the variant's import (a second FROM
        // line), never orphan fragments on their own.
        if (base.contains(QLatin1String("mmproj"), Qt::CaseInsensitive)
                || base.contains(QLatin1String("projector"), Qt::CaseInsensitive)) {
            continue;
        }
        if (dir.exists(base + QStringLiteral(".omm.json")))
            continue;
        // A projector/other shard of a variant whose sidecar is named after
        // its first file: still part of that import, not an orphan.
        if (coveredFiles.contains(base))
            continue;
        bool covered = false;
        for (const HfIncomplete &known : std::as_const(out)) {
            if (known.label == base) {
                covered = true;
                break;
            }
        }
        if (covered)
            continue;
        HfIncomplete item;
        item.sidecar.clear();
        item.label = base;
        item.downloaded = QFileInfo(dir.filePath(name)).size();
        out.append(item);
    }
    return out;
}

bool DiscoverPanel::resumeHfIncomplete(const QString &sidecar)
{
    QFile file(sidecar);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    HfVariant variant;
    variant.id = object.value(QStringLiteral("id")).toString();
    variant.label = object.value(QStringLiteral("label")).toString();
    variant.size = qint64(object.value(QStringLiteral("total")).toDouble());
    const QJsonArray files = object.value(QStringLiteral("files")).toArray();
    const QJsonArray sizes = object.value(QStringLiteral("sizes")).toArray();
    for (int i = 0; i < files.size(); ++i) {
        variant.files.append(files.at(i).toString());
        variant.sizes.append(qint64(sizes.at(i).toDouble()));
    }
    if (variant.id.isEmpty() || variant.files.isEmpty())
        return false;
    m_pendingItemId = object.value(QStringLiteral("itemId")).toString();
    startHfDownload(object.value(QStringLiteral("repo")).toString(), variant);
    return true;
}

QString DiscoverPanel::downloadsDir() const
{
    return defaultDownloadDir();
}

bool DiscoverPanel::removeHfPartial(const QString &partPath)
{
    if (!partPath.endsWith(QLatin1String(".part")))
        return false;
    if (m_hfActiveSources.contains(partPath.left(partPath.size() - 5)))
        return false;  // source of the running `ollama create`
    const bool removed = QFile::remove(partPath);
    QFile::remove(partPath + QStringLiteral(".rev"));
    return removed;
}

bool DiscoverPanel::removeHfIncomplete(const QString &sidecar)
{
    QFile file(sidecar);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    const QString dir = defaultDownloadDir();
    for (const QJsonValue &value : object.value(QStringLiteral("files")).toArray()) {
        const QString base = QDir(dir).filePath(QFileInfo(value.toString()).fileName());
        if (m_hfActiveSources.contains(base))
            continue;  // needed by the running `ollama create`
        QFile::remove(base);
        QFile::remove(base + QStringLiteral(".part"));
        QFile::remove(base + QStringLiteral(".part.rev"));
    }
    file.close();
    QFile::remove(sidecar);
    return true;
}

bool DiscoverPanel::moveHfTaskBefore(const QString &draggedId, const QString &targetId)
{
    int from = -1;
    int to = -1;
    for (int i = 0; i < m_hfQueue.size(); ++i) {
        if (m_hfQueue.at(i).variant.id == draggedId)
            from = i;
        if (m_hfQueue.at(i).variant.id == targetId)
            to = i;
    }
    if (from < 0 || to < 0 || from == to)
        return false;
    if (from < to)
        --to;
    m_hfQueue.move(from, to);
    saveHfQueue();
    notifyHfActivity();
    return true;
}

bool DiscoverPanel::prioritizeHfTask(const QString &id)
{
    int index = -1;
    for (int i = 0; i < m_hfQueue.size(); ++i) {
        if (m_hfQueue.at(i).variant.id == id) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return false;

    const HfPending target = m_hfQueue.takeAt(index);
    if (m_queueActive && !m_createJob) {
        // Stop the running download and put it right behind the promoted one;
        // partial shards stay, so the demoted import resumes from its range.
        HfPending current;
        current.repo = m_pendingRepo;
        current.itemId = m_activeHfItemId;
        current.variant = m_activeHfVariant;
        if (current.variant.id.isEmpty()) {
            // Defensive: the running task may predate the cached variant.
            current.variant.id = m_activeHfVariantId;
            current.variant.files = m_queueFiles;
            current.variant.sizes = m_queueSizes;
            current.variant.size = m_queueTotalBytes;
        }
        m_hfQueue.prepend(current);
        m_hfQueue.prepend(target);
        m_panel->setVariantJobState(current.itemId, current.variant.id,
                                    ModelBrowserPanel::JobState::Queued);
        m_panel->setVariantJobState(target.itemId, target.variant.id,
                                    ModelBrowserPanel::JobState::Queued);
        // advanceHfQueue() must not clear the demoted task's badge.
        m_activeHfVariantId.clear();
        m_activeHfItemId.clear();
        // The .part stays for the later resume, but the "incomplete" sidecar
        // must not: while queued the task is simply "waiting", and the sidecar
        // is rewritten by startHfDownload when it runs again.
        if (!current.variant.id.isEmpty()) {
            QFile::remove(QDir(defaultDownloadDir()).filePath(
                    QFileInfo(current.variant.id).fileName() + QStringLiteral(".omm.json")));
        }
        // Stop the running reply and ignore its "cancelled" signal: the next
        // task is started right here, so the transition cannot interleave with
        // another reorder.
        m_hfIgnoreCancel = true;
        m_cancelRequested = false;
        m_hf->cancelDownload();
        stopHfQueue();
        for (int i = 0; i < m_hfQueue.size(); ++i) {
            if (m_hfQueue.at(i).variant.id != target.variant.id)
                continue;
            const HfPending next = m_hfQueue.takeAt(i);
            m_pendingItemId = next.itemId;
            startHfDownload(next.repo, next.variant);
            break;
        }
        QTimer::singleShot(5000, this, [this]() { m_hfIgnoreCancel = false; });
        saveHfQueue();
        notifyHfActivity();
        return true;
    }

    // Nothing to preempt (create step or idle): it will start next.
    m_hfQueue.prepend(target);
    notifyHfActivity();
    if (!m_queueActive && !m_createJob)
        advanceHfQueue();
    return true;
}

bool DiscoverPanel::cancelHfTask(const QString &id)
{
    if (isOllama())
        return false;
    if (id == m_activeHfVariantId) {
        if (m_queueActive && !m_createJob) {
            onCancel();
            return true;
        }
        return false;
    }
    for (int i = 0; i < m_hfQueue.size(); ++i) {
        if (m_hfQueue.at(i).variant.id != id)
            continue;
        const HfPending removed = m_hfQueue.takeAt(i);
        QFile::remove(QDir(defaultDownloadDir()).filePath(
                QFileInfo(removed.variant.id).fileName() + QStringLiteral(".omm.json")));
        m_panel->setVariantJobState(removed.itemId, removed.variant.id,
                                    ModelBrowserPanel::JobState::None);
        saveHfQueue();
        notifyHfActivity();
        if (!m_queueActive && !m_createJob)
            m_panel->setBusy(false, 0);
        return true;
    }
    return false;
}

void DiscoverPanel::notifyHfActivity()
{
    if (isOllama())
        return;
    emit hfActivityChanged((m_queueActive || m_createJob ? 1 : 0) + m_hfQueue.size());
}

void DiscoverPanel::writeHfSidecar(const QString &path, const QString &repo,
                                  const HfVariant &variant) const
{
    QJsonObject object;
    object.insert(QStringLiteral("repo"), repo);
    object.insert(QStringLiteral("itemId"), m_pendingItemId);
    object.insert(QStringLiteral("id"), variant.id);
    object.insert(QStringLiteral("label"), variant.label);
    object.insert(QStringLiteral("total"), double(variant.size));
    QJsonArray files;
    QJsonArray sizes;
    for (int i = 0; i < variant.files.size(); ++i) {
        files.append(variant.files.at(i));
        sizes.append(double(variant.sizes.value(i)));
    }
    object.insert(QStringLiteral("files"), files);
    object.insert(QStringLiteral("sizes"), sizes);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

void DiscoverPanel::removeHfPartialSources()
{
    const QString dir = defaultDownloadDir();
    for (const QString &file : std::as_const(m_queueFiles)) {
        const QString base = QDir(dir).filePath(QFileInfo(file).fileName());
        QFile::remove(base);                 // finished shard of a split model
        QFile::remove(base + QStringLiteral(".part"));
        QFile::remove(base + QStringLiteral(".part.rev"));
    }
    if (!m_queueFirstFile.isEmpty()) {
        QFile::remove(QDir(defaultDownloadDir()).filePath(
                QFileInfo(m_queueFirstFile).fileName() + QStringLiteral(".omm.json")));
    }
}

void DiscoverPanel::advanceHfQueue()
{
    if (!m_activeHfVariantId.isEmpty()) {
        m_panel->setVariantJobState(m_activeHfItemId, m_activeHfVariantId,
                                    ModelBrowserPanel::JobState::None);
        m_activeHfVariantId.clear();
        m_activeHfItemId.clear();
    }
    if (!m_hfQueue.isEmpty()) {
        const HfPending next = m_hfQueue.takeFirst();
        m_pendingItemId = next.itemId;
        startHfDownload(next.repo, next.variant);
        return;
    }
    m_panel->setBusy(false, 0);
    notifyHfActivity();
}

void DiscoverPanel::cancelHfActivity()
{
    if (!isOllama())
        onCancel();
}
