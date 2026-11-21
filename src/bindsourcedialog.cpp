#include "bindsourcedialog.h"

#include "designtokens.h"
#include "hfauth.h"
#include "i18n.h"
#include "provenance.h"
#include "quantmatch.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString humanSize(qint64 bytes)
{
    constexpr double gb = 1024.0 * 1024.0 * 1024.0;
    if (bytes <= 0)
        return QString();
    const double value = double(bytes) / gb;
    if (value >= 1.0)
        return QString::number(value, 'f', value < 10.0 ? 1 : 0) + QStringLiteral(" GB");
    return QString::number(double(bytes) / (1024.0 * 1024.0), 'f', 0) + QStringLiteral(" MB");
}

} // namespace

// ---------------------------------------------------------------- auto binder

SourceAutoBinder::SourceAutoBinder(QObject *parent)
    : QObject(parent)
    , m_registry(new RegistryClient(this))
{
    connect(m_registry, &RegistryClient::tagsFinished, this, &SourceAutoBinder::onTags);
    connect(m_registry, &RegistryClient::manifestChecked, this, &SourceAutoBinder::onManifest);
}

void SourceAutoBinder::setTargets(const QList<BindTarget> &targets)
{
    m_targets = targets;
}

QString SourceAutoBinder::currentModel() const
{
    return (m_index >= 0 && m_index < m_targets.size()) ? m_targets.at(m_index).modelName
                                                        : QString();
}

void SourceAutoBinder::start()
{
    if (m_running || m_targets.isEmpty())
        return;
    m_running = true;
    m_cancelled = false;
    m_index = -1;
    m_boundCount = 0;
    nextModel();
}

void SourceAutoBinder::cancel()
{
    if (!m_running)
        return;
    m_cancelled = true;
    m_running = false;
    emit finished(m_boundCount, m_targets.size());
}

void SourceAutoBinder::nextModel()
{
    if (!m_running)
        return;

    ++m_index;
    if (m_index >= m_targets.size()) {
        m_running = false;
        emit finished(m_boundCount, m_targets.size());
        return;
    }

    const BindTarget target = m_targets.at(m_index);
    m_tags.clear();
    m_tagIndex = 0;
    m_checked = 0;
    m_total = 0;
    m_families.clear();

    if (target.localId.trimmed().isEmpty()) {
        finishTarget(false);
        return;
    }

    // A model is usually "family:tag"; a locally created one may only carry a
    // size/quant suffix ("qwen3-8b-q4_k_m"), so the stem is tried as well.
    const QString family = target.modelName.section(QLatin1Char(':'), 0, 0).trimmed();
    if (!family.isEmpty())
        m_families << family;
    const QString stem = stripQuantSuffix(family);
    if (!stem.isEmpty() && stem != family)
        m_families << stem;
    if (m_families.isEmpty()) {
        finishTarget(false);
        return;
    }
    nextFamily();
}

void SourceAutoBinder::nextFamily()
{
    if (!m_running)
        return;
    if (m_families.isEmpty()) {
        finishTarget(false);
        return;
    }
    m_tags.clear();
    m_tagIndex = 0;
    m_checked = 0;
    m_total = 0;
    m_registry->listTags(m_families.takeFirst());
}

void SourceAutoBinder::onTags(bool ok, const QList<RegistryClient::Tag> &tags,
                              const QString &error)
{
    Q_UNUSED(error);
    if (!m_running)
        return;
    if (!ok || tags.isEmpty()) {
        nextFamily();
        return;
    }
    m_tags = tags;
    m_tagIndex = 0;
    m_total = tags.size();
    nextTag();
}

void SourceAutoBinder::nextTag()
{
    if (!m_running)
        return;
    if (m_tagIndex >= m_tags.size()) {
        nextFamily();
        return;
    }
    const QString ref = m_tags.at(m_tagIndex).name;
    ++m_tagIndex;
    m_checked = m_tagIndex;
    emit progress(currentModel(), m_checked, m_total);
    m_registry->checkManifest(ref);
}

void SourceAutoBinder::onManifest(const QString &ref, const QString &digest, bool ok,
                                  const QString &error)
{
    Q_UNUSED(error);
    if (!m_running)
        return;
    const BindTarget target = m_targets.at(m_index);
    if (!ok || digest.isEmpty() || !digestMatches(target.localId, digest)) {
        nextTag();
        return;
    }

    // `ollama cp` does not change the manifest digest, so the tag with our
    // digest is where this model came from even after a rename.
    Provenance::Entry entry;
    entry.source = QStringLiteral("ollama");
    entry.ref = ref;
    entry.digest = digest;
    // Never rewrite when the model was installed: only a missing value is new.
    entry.installedAt = Provenance::instance().lookup(target.modelName).installedAt;
    Provenance::instance().record(target.modelName, entry);
    emit bound(target.modelName, ref, digest);
    ++m_boundCount;
    finishTarget(true);
}

void SourceAutoBinder::finishTarget(bool success)
{
    if (!success && m_index >= 0 && m_index < m_targets.size())
        emit targetFailed(m_targets.at(m_index).modelName);
    nextModel();
}

// --------------------------------------------------------------------- dialog

BindSourceDialog::BindSourceDialog(const QString &modelName, const QString &localId,
                                   QWidget *parent)
    : QDialog(parent)
    , m_modelName(modelName)
    , m_localId(localId)
    , m_registry(new RegistryClient(this))
    , m_hf(new HfClient(this))
{
    // Gated HuggingFace repositories need the same token the browser uses.
    m_hf->setToken(HfAuth::token());
    setupUi();

    connect(m_registry, &RegistryClient::tagsFinished, this,
            &BindSourceDialog::onOllamaTags);
    connect(m_registry, &RegistryClient::searchFinished, this,
            [this](bool ok, const QList<RegistryClient::Model> &models, const QString &error) {
        m_ollamaModels->clear();
        if (!ok) {
            m_statusLabel->setText(T("bind.error").arg(error));
            return;
        }
        for (const RegistryClient::Model &model : models) {
            auto *item = new QListWidgetItem(model.name, m_ollamaModels);
            item->setToolTip(model.description);
        }
        m_statusLabel->setText(T("search.modelsFound").arg(models.size()));
        // Selecting the first family fetches its tags (currentItemChanged).
        if (m_ollamaModels->count() > 0)
            m_ollamaModels->setCurrentRow(0);
    });
    connect(m_hf, &HfClient::searchFinished, this, &BindSourceDialog::onHfSearchFinished);
    connect(m_hf, &HfClient::filesFinished, this, &BindSourceDialog::onHfFiles);
    connect(m_hf, &HfClient::repoInfoFinished, this, &BindSourceDialog::onHfRepoInfo);

    retranslateUi();
    connect(I18n::instance(), &I18n::languageChanged, this, &BindSourceDialog::retranslateUi);

    // The search is pre-filled with the model stem so one Enter finds the family.
    const QString stem = bindSearchStem(m_modelName);
    m_ollamaSearchEdit->setText(stem);
    m_hfSearchEdit->setText(stem);
    QTimer::singleShot(0, this, &BindSourceDialog::startOllamaSearch);
}

void BindSourceDialog::setupUi()
{
    setWindowModality(Qt::WindowModal);
    resize(780, 560);

    auto *layout = new QVBoxLayout(this);

    m_modelLabel = new QLabel(m_modelName);
    QFont bold = m_modelLabel->font();
    bold.setBold(true);
    m_modelLabel->setFont(bold);
    layout->addWidget(m_modelLabel);

    m_statusLabel = new QLabel();
    m_statusLabel->setObjectName(QStringLiteral("bindStatus"));
    tokens::setMuted(m_statusLabel);
    tokens::setCaption(m_statusLabel);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    m_tabs = new QTabWidget();
    layout->addWidget(m_tabs, 1);

    // ---- Ollama tab
    auto *ollamaPage = new QWidget();
    auto *ollamaLayout = new QVBoxLayout(ollamaPage);

    auto *autoRow = new QHBoxLayout();
    m_autoBtn = new QPushButton();
    m_autoBtn->setObjectName(QStringLiteral("bindAutoBtn"));
    autoRow->addWidget(m_autoBtn);
    m_autoCancelBtn = new QPushButton();
    m_autoCancelBtn->setVisible(false);
    autoRow->addWidget(m_autoCancelBtn);
    autoRow->addStretch();
    ollamaLayout->addLayout(autoRow);

    m_autoLabel = new QLabel();
    m_autoLabel->setObjectName(QStringLiteral("bindAutoLabel"));
    tokens::setMuted(m_autoLabel);
    tokens::setCaption(m_autoLabel);
    m_autoLabel->setVisible(false);
    ollamaLayout->addWidget(m_autoLabel);

    m_autoBar = new QProgressBar();
    m_autoBar->setObjectName(QStringLiteral("bindAutoBar"));
    m_autoBar->setRange(0, 100);
    m_autoBar->setValue(0);
    m_autoBar->setTextVisible(false);
    m_autoBar->setVisible(false);
    ollamaLayout->addWidget(m_autoBar);

    auto *ollamaSearchRow = new QHBoxLayout();
    m_ollamaSearchEdit = new QLineEdit();
    m_ollamaSearchEdit->setClearButtonEnabled(true);
    ollamaSearchRow->addWidget(m_ollamaSearchEdit, 1);
    m_ollamaSearchBtn = new QPushButton();
    ollamaSearchRow->addWidget(m_ollamaSearchBtn);
    ollamaLayout->addLayout(ollamaSearchRow);

    auto *ollamaLists = new QHBoxLayout();
    auto *modelsColumn = new QVBoxLayout();
    m_ollamaResultsLabel = new QLabel();
    tokens::setMuted(m_ollamaResultsLabel);
    tokens::setCaption(m_ollamaResultsLabel);
    modelsColumn->addWidget(m_ollamaResultsLabel);
    m_ollamaModels = new QListWidget();
    m_ollamaModels->setObjectName(QStringLiteral("bindOllamaModels"));
    modelsColumn->addWidget(m_ollamaModels, 1);
    ollamaLists->addLayout(modelsColumn, 1);

    auto *tagsColumn = new QVBoxLayout();
    m_ollamaTagsLabel = new QLabel();
    tokens::setMuted(m_ollamaTagsLabel);
    tokens::setCaption(m_ollamaTagsLabel);
    tagsColumn->addWidget(m_ollamaTagsLabel);
    m_ollamaTags = new QListWidget();
    m_ollamaTags->setObjectName(QStringLiteral("bindOllamaTags"));
    tagsColumn->addWidget(m_ollamaTags, 1);
    ollamaLists->addLayout(tagsColumn, 1);
    ollamaLayout->addLayout(ollamaLists, 1);

    m_ollamaBindBtn = new QPushButton();
    m_ollamaBindBtn->setProperty("accent", true);
    m_ollamaBindBtn->setEnabled(false);
    ollamaLayout->addWidget(m_ollamaBindBtn, 0, Qt::AlignRight);

    m_tabs->addTab(ollamaPage, QString());

    // ---- HuggingFace tab
    auto *hfPage = new QWidget();
    auto *hfLayout = new QVBoxLayout(hfPage);

    auto *hfSearchRow = new QHBoxLayout();
    m_hfSearchEdit = new QLineEdit();
    m_hfSearchEdit->setClearButtonEnabled(true);
    hfSearchRow->addWidget(m_hfSearchEdit, 1);
    m_hfSearchBtn = new QPushButton();
    hfSearchRow->addWidget(m_hfSearchBtn);
    hfLayout->addLayout(hfSearchRow);

    auto *hfLists = new QHBoxLayout();
    auto *reposColumn = new QVBoxLayout();
    m_hfReposLabel = new QLabel();
    tokens::setMuted(m_hfReposLabel);
    tokens::setCaption(m_hfReposLabel);
    reposColumn->addWidget(m_hfReposLabel);
    m_hfRepos = new QListWidget();
    m_hfRepos->setObjectName(QStringLiteral("bindHfRepos"));
    reposColumn->addWidget(m_hfRepos, 1);
    hfLists->addLayout(reposColumn, 1);

    auto *filesColumn = new QVBoxLayout();
    m_hfFilesLabel = new QLabel();
    tokens::setMuted(m_hfFilesLabel);
    tokens::setCaption(m_hfFilesLabel);
    filesColumn->addWidget(m_hfFilesLabel);
    m_hfFiles = new QListWidget();
    m_hfFiles->setObjectName(QStringLiteral("bindHfFiles"));
    filesColumn->addWidget(m_hfFiles, 1);
    hfLists->addLayout(filesColumn, 1);
    hfLayout->addLayout(hfLists, 1);

    m_hfBindBtn = new QPushButton();
    m_hfBindBtn->setProperty("accent", true);
    m_hfBindBtn->setEnabled(false);
    hfLayout->addWidget(m_hfBindBtn, 0, Qt::AlignRight);

    m_tabs->addTab(hfPage, QString());

    auto *buttons = new QDialogButtonBox();
    QPushButton *closeBtn = buttons->addButton(T("common.close"),
                                               QDialogButtonBox::RejectRole);
    Q_UNUSED(closeBtn);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_tabs, &QTabWidget::currentChanged, this, &BindSourceDialog::onTabChanged);
    connect(m_autoBtn, &QPushButton::clicked, this, &BindSourceDialog::onAutoDetect);
    connect(m_autoCancelBtn, &QPushButton::clicked, this, [this]() {
        if (m_binder)
            m_binder->cancel();
    });
    connect(m_ollamaSearchBtn, &QPushButton::clicked, this, &BindSourceDialog::onOllamaSearch);
    connect(m_ollamaSearchEdit, &QLineEdit::returnPressed, this,
            &BindSourceDialog::onOllamaSearch);
    connect(m_ollamaModels, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) { onOllamaModelActivated(); });
    connect(m_ollamaTags, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
        m_ollamaBindBtn->setEnabled(current != nullptr);
    });
    connect(m_ollamaBindBtn, &QPushButton::clicked, this, &BindSourceDialog::onBindOllamaTag);

    connect(m_hfSearchBtn, &QPushButton::clicked, this, &BindSourceDialog::onHfSearch);
    connect(m_hfSearchEdit, &QLineEdit::returnPressed, this, &BindSourceDialog::onHfSearch);
    connect(m_hfRepos, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
        if (current)
            onHfRepoSelected();
    });
    connect(m_hfFiles, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
        m_hfBindBtn->setEnabled(current != nullptr);
    });
    connect(m_hfBindBtn, &QPushButton::clicked, this, &BindSourceDialog::onBindHfFile);
}

void BindSourceDialog::retranslateUi()
{
    setWindowTitle(T("bind.title"));
    m_modelLabel->setText(m_modelName);
    m_tabs->setTabText(0, T("bind.ollamaTab"));
    m_tabs->setTabText(1, T("bind.hfTab"));

    m_autoBtn->setText(T("bind.auto"));
    m_autoBtn->setToolTip(T("bind.autoTip"));
    m_autoCancelBtn->setText(T("browser.cancel"));

    m_ollamaSearchEdit->setPlaceholderText(T("bind.searchHint"));
    m_hfSearchEdit->setPlaceholderText(T("bind.searchHint"));
    m_ollamaSearchBtn->setText(T("search.search"));
    m_hfSearchBtn->setText(T("search.search"));
    m_ollamaResultsLabel->setText(T("bind.results"));
    m_ollamaTagsLabel->setText(T("bind.tags"));
    m_hfReposLabel->setText(T("bind.repos"));
    m_hfFilesLabel->setText(T("bind.files"));
    m_ollamaBindBtn->setText(T("bind.bind"));
    m_hfBindBtn->setText(T("bind.bind"));

    if (!m_binder)
        m_statusLabel->setText(T("bind.unknownSource"));
}

void BindSourceDialog::onTabChanged(int index)
{
    if (index == 1 && !m_hfSearched)
        startHfSearch();
}

void BindSourceDialog::startOllamaSearch()
{
    const QString query = m_ollamaSearchEdit->text().trimmed();
    if (query.isEmpty()) {
        m_statusLabel->setText(T("search.emptyQuery"));
        return;
    }
    m_ollamaSearched = true;
    m_statusLabel->setText(T("search.searching"));
    m_registry->search(query);
}

void BindSourceDialog::onOllamaSearch()
{
    startOllamaSearch();
}

void BindSourceDialog::onOllamaModelActivated()
{
    QListWidgetItem *item = m_ollamaModels->currentItem();
    if (!item)
        return;
    m_tags.clear();
    m_ollamaTags->clear();
    m_ollamaBindBtn->setEnabled(false);
    m_statusLabel->setText(T("search.loadingTags"));
    m_registry->listTags(item->text());
}

void BindSourceDialog::onOllamaTags(bool ok, const QList<RegistryClient::Tag> &tags,
                                    const QString &error)
{
    m_ollamaTags->clear();
    m_ollamaBindBtn->setEnabled(false);
    if (!ok) {
        m_statusLabel->setText(T("bind.error").arg(error));
        return;
    }
    m_tags = tags;
    for (const RegistryClient::Tag &tag : tags) {
        QString text = tag.name;
        if (!tag.size.isEmpty())
            text += QStringLiteral("   ") + tag.size;
        if (!tag.context.isEmpty())
            text += QStringLiteral("  ·  ") + tag.context;
        auto *item = new QListWidgetItem(text, m_ollamaTags);
        item->setData(Qt::UserRole, tag.name);
    }
    m_statusLabel->setText(T("search.tagsFound").arg(tags.size()));
    if (m_ollamaTags->count() > 0)
        m_ollamaTags->setCurrentRow(0);
}

void BindSourceDialog::onBindOllamaTag()
{
    QListWidgetItem *item = m_ollamaTags->currentItem();
    if (!item)
        return;
    const QString ref = item->data(Qt::UserRole).toString();
    if (ref.isEmpty())
        return;
    writeEntry(QStringLiteral("ollama"), ref, QString(), QString(), QString());
}

void BindSourceDialog::startHfSearch()
{
    const QString query = m_hfSearchEdit->text().trimmed();
    if (query.isEmpty()) {
        m_statusLabel->setText(T("search.emptyQuery"));
        return;
    }
    m_hfSearched = true;
    m_statusLabel->setText(T("search.searching"));
    m_hf->search(query);
}

void BindSourceDialog::onHfSearch()
{
    startHfSearch();
}

void BindSourceDialog::onHfSearchFinished(bool ok, const QList<HfClient::Repo> &repos,
                                          const QString &error)
{
    m_hfRepos->clear();
    m_hfFiles->clear();
    m_hfBindBtn->setEnabled(false);
    if (!ok) {
        m_statusLabel->setText(T("bind.error").arg(error));
        return;
    }
    for (const HfClient::Repo &repo : repos) {
        auto *item = new QListWidgetItem(repo.id, m_hfRepos);
        item->setToolTip(QStringLiteral("↓ %1 · ♥ %2").arg(repo.downloads).arg(repo.likes));
    }
    m_statusLabel->setText(T("search.modelsFound").arg(repos.size()));
    if (m_hfRepos->count() > 0)
        m_hfRepos->setCurrentRow(0);
}

void BindSourceDialog::onHfRepoSelected()
{
    QListWidgetItem *item = m_hfRepos->currentItem();
    if (!item)
        return;
    m_hfRepo = item->text();
    m_hfSha.clear();
    m_hfFiles->clear();
    m_hfBindBtn->setEnabled(false);
    m_statusLabel->setText(T("search.loadingFiles"));
    m_hf->listFiles(m_hfRepo);
    m_hf->fetchRepoInfo(m_hfRepo);
}

void BindSourceDialog::onHfFiles(bool ok, const QList<HfClient::File> &files,
                                 const QString &error)
{
    m_hfFiles->clear();
    m_hfFileNames.clear();
    m_hfBindBtn->setEnabled(false);
    if (!ok) {
        m_statusLabel->setText(T("bind.error").arg(error));
        return;
    }
    QList<HfClient::File> gguf;
    for (const HfClient::File &file : files) {
        // A split GGUF is one logical file: list its first part only.
        if (!file.isGguf() || (file.split && file.part > 1))
            continue;
        gguf << file;
    }
    for (const HfClient::File &file : gguf) {
        QString text = file.name;
        const QString size = humanSize(file.size);
        if (!size.isEmpty())
            text += QStringLiteral("   ") + size;
        auto *item = new QListWidgetItem(text, m_hfFiles);
        item->setData(Qt::UserRole, file.name);
    }
    m_statusLabel->setText(T("search.filesFound").arg(gguf.size()));

    const QString preferred = preferredHfFile(gguf);
    for (int i = 0; i < m_hfFiles->count(); ++i) {
        if (m_hfFiles->item(i)->data(Qt::UserRole).toString() == preferred) {
            m_hfFiles->setCurrentRow(i);
            break;
        }
    }
    if (m_hfFiles->count() > 0 && !m_hfFiles->currentItem())
        m_hfFiles->setCurrentRow(0);
}

QString BindSourceDialog::preferredHfFile(const QList<HfClient::File> &files) const
{
    if (files.isEmpty())
        return QString();
    const QString tagPart = m_modelName.section(QLatin1Char(':'), 1);
    if (!tagPart.isEmpty()) {
        for (const HfClient::File &file : files) {
            if (!file.quant.isEmpty() && file.quant.compare(tagPart, Qt::CaseInsensitive) == 0)
                return file.name;
        }
    }
    const QString stem = bindSearchStem(m_modelName);
    for (const HfClient::File &file : files) {
        if (!stem.isEmpty() && file.name.contains(stem, Qt::CaseInsensitive))
            return file.name;
    }
    return files.first().name;
}

void BindSourceDialog::onHfRepoInfo(const QString &repo, const QString &sha, bool ok,
                                    const QString &error)
{
    if (repo != m_hfRepo)
        return;
    if (!ok) {
        m_hfSha.clear();
        if (m_hfBindPending) {
            m_hfBindPending = false;
            m_statusLabel->setText(T("bind.error").arg(error));
        }
        return;
    }
    m_hfSha = sha;
    if (m_hfBindPending) {
        m_hfBindPending = false;
        onBindHfFile();
    }
}

void BindSourceDialog::onBindHfFile()
{
    QListWidgetItem *item = m_hfFiles->currentItem();
    if (!item || m_hfRepo.isEmpty())
        return;
    const QString file = item->data(Qt::UserRole).toString();
    if (file.isEmpty())
        return;
    if (m_hfSha.isEmpty()) {
        // The commit sha is required for update checks: fetch it first.
        m_hfBindPending = true;
        m_statusLabel->setText(T("bind.loadingSha"));
        m_hf->fetchRepoInfo(m_hfRepo);
        return;
    }
    writeEntry(QStringLiteral("hf"), m_hfRepo, file, QString(), m_hfSha);
}

void BindSourceDialog::writeEntry(const QString &source, const QString &ref,
                                  const QString &file, const QString &digest,
                                  const QString &sha)
{
    Provenance::Entry entry;
    entry.source = source;
    entry.ref = ref;
    entry.file = file;
    entry.digest = digest;
    entry.sha = sha;
    // Keep the installation date of an existing entry: binding only restores
    // the origin, it does not re-install the model.
    entry.installedAt = Provenance::instance().lookup(m_modelName).installedAt;
    Provenance::instance().record(m_modelName, entry);
    m_statusLabel->setText(T("bind.bound").arg(ref.isEmpty() ? m_modelName : ref));
    emit bound(m_modelName);
    accept();
}

void BindSourceDialog::onAutoDetect()
{
    if (m_binder)
        return;
    if (m_localId.trimmed().isEmpty()) {
        m_autoLabel->setVisible(true);
        m_autoLabel->setText(T("bind.autoFailed"));
        QMessageBox::information(this, T("bind.title"), T("bind.autoFailed"));
        return;
    }

    m_binder = new SourceAutoBinder(this);
    connect(m_binder, &SourceAutoBinder::progress, this, &BindSourceDialog::onAutoProgress);
    connect(m_binder, &SourceAutoBinder::bound, this, &BindSourceDialog::onAutoBound);
    connect(m_binder, &SourceAutoBinder::targetFailed, this, [this](const QString &) {
        m_statusLabel->setText(T("bind.autoFailed"));
    });
    connect(m_binder, &SourceAutoBinder::finished, this, &BindSourceDialog::onAutoFinished);

    BindTarget target;
    target.modelName = m_modelName;
    target.localId = m_localId;
    m_binder->setTargets({target});
    setAutoRunning(true);
    m_autoLabel->setText(T("bind.autoRunning"));
    m_binder->start();
}

void BindSourceDialog::onAutoProgress(const QString &modelName, int checked, int total)
{
    Q_UNUSED(modelName);
    m_autoBar->setRange(0, total > 0 ? total : 1);
    m_autoBar->setValue(checked);
    m_autoLabel->setText(T("bind.autoProgress").arg(checked).arg(total));
}

void BindSourceDialog::onAutoBound(const QString &modelName, const QString &ref,
                                   const QString &digest)
{
    Q_UNUSED(digest);
    m_statusLabel->setText(T("bind.bound").arg(ref));
    emit bound(modelName);
    accept();
}

void BindSourceDialog::onAutoFinished(int boundCount, int targetCount)
{
    Q_UNUSED(targetCount);
    setAutoRunning(false);
    if (boundCount == 0) {
        m_autoLabel->setVisible(true);
        m_autoLabel->setText(T("bind.autoFailed"));
        QMessageBox::information(this, T("bind.title"), T("bind.autoFailed"));
    }
    if (m_binder) {
        m_binder->deleteLater();
        m_binder = nullptr;
    }
}

void BindSourceDialog::setAutoRunning(bool running)
{
    m_autoBtn->setEnabled(!running);
    m_autoCancelBtn->setVisible(running);
    m_autoLabel->setVisible(running);
    m_autoBar->setVisible(running);
    if (running) {
        m_autoBar->setRange(0, 1);
        m_autoBar->setValue(0);
    }
}
