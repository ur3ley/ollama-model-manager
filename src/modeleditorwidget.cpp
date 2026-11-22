#include "modeleditorwidget.h"

#include "combohighlightdelegate.h"
#include "popupcombobox.h"

#include <QAbstractItemView>
#include <QMenu>
#include <QScrollArea>
#include <QMessageBox>
#include <QApplication>
#include <QStyle>
#include <QGridLayout>
#include <QRegularExpressionValidator>
#include "designtokens.h"
#include "doticons.h"
#include "installservice.h"
#include "memoryinfo.h"
#include "quantmatch.h"

ModelEditorWidget::ModelEditorWidget(InstallService *install, QWidget *parent)
    : QWidget(parent)
    , m_install(install)
    , m_cli(install->cli())
    , m_registry(new RegistryClient(this))
{
    // FROM is editable: wait for typing to settle before asking `ollama show`.
    // Created before setupUI(), which may already change the FROM text.
    m_ctxTimer = new QTimer(this);
    m_ctxTimer->setSingleShot(true);
    m_ctxTimer->setInterval(250);
    connect(m_ctxTimer, &QTimer::timeout, this, [this]() {
        if (m_ctxBase.isEmpty())
            return;
        OllamaJob *job = m_cli->showModelAsync(m_ctxBase);
        m_ctxJob = job;
        connect(job, &OllamaJob::finished, this, [this, job](bool ok, const QString &) {
            // A newer FROM (or a cleared one) superseded this request.
            if (job != m_ctxJob)
                return;
            m_ctxJob.clear();
            m_ctxBase.clear();
            applyBaseContext(ok, job->params());
        });
    });
    setupUI();

    connect(theme::Theme::instance(), &theme::Theme::changed, this, [this]() {
        applyFromComboColors();
    });

    connect(m_registry, &RegistryClient::tagsFinished, this,
            [this](bool ok, const QList<RegistryClient::Tag> &tags, const QString &error) {
        onFamilyTags(ok, tags, error);
    });
    // Any finished delete (ours or the list's) may remove an entry of the
    // FROM picker; this is a view refresh, not a result we wait for.
    connect(m_cli, &OllamaCli::jobFinished, this, [this](OllamaJob *job) {
        if (job->kind() != OllamaJob::Kind::Delete)
            return;
        rebuildFromCombo(currentFromName());
        updateNameForSelection();
    });
}

void ModelEditorWidget::setupUI()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    m_titleLabel = new QLabel();
    QFont boldFont = m_titleLabel->font();
    boldFont.setBold(true);
    boldFont.setPointSize(boldFont.pointSize() + 2);
    m_titleLabel->setFont(boldFont);
    mainLayout->addWidget(m_titleLabel);

    // Second title line: repository · owner (the ref goes into the tooltip).
    m_sourceLabel = new QLabel();
    tokens::setMuted(m_sourceLabel);
    tokens::setCaption(m_sourceLabel);
    m_sourceLabel->setVisible(false);
    mainLayout->addWidget(m_sourceLabel);

    // Local model: provenance is unknown, offer to restore it without a reinstall.
    auto *bindRow = new QHBoxLayout();
    m_unknownSourceLabel = new QLabel();
    tokens::setMuted(m_unknownSourceLabel);
    tokens::setCaption(m_unknownSourceLabel);
    m_unknownSourceLabel->setWordWrap(true);
    m_unknownSourceLabel->setVisible(false);
    bindRow->addWidget(m_unknownSourceLabel, 1);
    m_bindSourceBtn = new QPushButton();
    m_bindSourceBtn->setVisible(false);
    bindRow->addWidget(m_bindSourceBtn);
    mainLayout->addLayout(bindRow);
    connect(m_bindSourceBtn, &QPushButton::clicked, this, [this]() {
        QString name = m_currentEditModel;
        if (name.isEmpty() && !m_repo.quants.isEmpty())
            name = m_repo.quants.first().modelName;
        if (!name.isEmpty())
            emit bindSourceRequested(name);
    });

    auto *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *scrollWidget = new QWidget();
    auto *form = new QVBoxLayout(scrollWidget);

    m_modelGroup = new QGroupBox();
    m_modelLayout = new QFormLayout(m_modelGroup);
    // Every field takes the full remaining width; otherwise the FROM combo and
    // the directive fields collapse to their size hint and look empty.
    m_modelLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_modelLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    m_modelLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    m_fromCombo = new PopupComboBox();
    m_fromCombo->setEditable(true);
    m_fromCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_fromCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_fromCombo->setContextMenuPolicy(Qt::CustomContextMenu);
    m_fromCombo->setObjectName(QStringLiteral("fromCombo"));
    m_fromCombo->view()->setMouseTracking(true);
    m_fromCombo->view()->setItemDelegate(new ComboHighlightDelegate(m_fromCombo->view()));

    // Delete sits right next to the quant picker: the action always targets the
    // quant selected in FROM, never the whole repository.
    auto *fromRow = new QWidget();
    auto *fromRowLayout = new QHBoxLayout(fromRow);
    fromRowLayout->setContentsMargins(0, 0, 0, 0);
    fromRowLayout->setSpacing(6);
    fromRowLayout->addWidget(m_fromCombo, 1);
    m_deleteQuantBtn = new QToolButton();
    m_deleteQuantBtn->setAutoRaise(true);
    m_deleteQuantBtn->setIcon(QApplication::style()->standardIcon(QStyle::SP_TrashIcon));
    m_deleteQuantBtn->setEnabled(false);
    fromRowLayout->addWidget(m_deleteQuantBtn);
    m_fromRowWidget = fromRow;
    m_modelLayout->addRow(new QLabel(), fromRow);

    m_nameEdit = new QLineEdit();
    m_nameEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[a-zA-Z0-9._/:\\-]*"), m_nameEdit));
    m_modelLayout->addRow(new QLabel(), m_nameEdit);

    m_rendererEdit = new QLineEdit();
    m_rendererEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[a-zA-Z0-9._\\-]*"), m_rendererEdit));
    m_modelLayout->addRow(new QLabel(), m_rendererEdit);

    m_parserEdit = new QLineEdit();
    m_parserEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[a-zA-Z0-9._\\-]*"), m_parserEdit));
    m_modelLayout->addRow(new QLabel(), m_parserEdit);

    // Read-only summary: what the model is built from and which renderer it uses.
    m_modelInfoLabel = new QLabel();
    m_modelInfoLabel->setWordWrap(true);
    tokens::setMuted(m_modelInfoLabel);
    tokens::setCaption(m_modelInfoLabel);
    m_modelInfoLabel->setTextFormat(Qt::PlainText);
    m_modelLayout->addRow(m_modelInfoLabel);

    m_advancedBtn = new QToolButton();
    m_advancedBtn->setObjectName(QStringLiteral("advancedBtn"));
    m_advancedBtn->setCheckable(true);
    m_advancedBtn->setChecked(false);
    m_advancedBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_advancedBtn->setArrowType(Qt::RightArrow);
    m_advancedBtn->setAutoRaise(true);
    m_modelLayout->addRow(QString(), m_advancedBtn);

    connect(m_advancedBtn, &QToolButton::toggled, this, [this](bool on) {
        m_advancedBtn->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
        setAdvancedVisible(on);
    });

    form->addWidget(m_modelGroup);

    m_capsLabel = new QLabel();
    tokens::setMuted(m_capsLabel);
    tokens::setCaption(m_capsLabel);
    m_capsLabel->setProperty("indent", true);
    m_capsLabel->setWordWrap(true);
    form->addWidget(m_capsLabel);

    m_templateWarnLabel = new QLabel();
    m_templateWarnLabel->setWordWrap(true);
    tokens::setRole(m_templateWarnLabel, "warning");
    tokens::setCaption(m_templateWarnLabel);
    m_templateWarnLabel->setProperty("indent", true);
    m_templateWarnLabel->setVisible(false);
    form->addWidget(m_templateWarnLabel);

    m_ctxGroup = new QGroupBox();
    auto *ctxLayout = new QVBoxLayout(m_ctxGroup);

    auto *ctxRow = new QHBoxLayout();
    m_numCtxCheck = new QCheckBox();
    m_numCtxCheck->setChecked(true);
    ctxRow->addWidget(m_numCtxCheck);

    m_numCtxSlider = new QSlider(Qt::Horizontal);
    m_numCtxSlider->setRange(2, 1024);
    m_numCtxSlider->setSingleStep(1);
    m_numCtxSlider->setPageStep(4);
    ctxRow->addWidget(m_numCtxSlider, 7);

    m_numCtxSpin = new QSpinBox();
    m_numCtxSpin->setRange(2, 1024);
    m_numCtxSpin->setSingleStep(1);
    ctxRow->addWidget(m_numCtxSpin, 3);

    m_ctxUnitLabel = new QLabel();
    ctxRow->addWidget(m_ctxUnitLabel);
    ctxLayout->addLayout(ctxRow);

    m_defaultCtxLabel = new QLabel();
    tokens::setMuted(m_defaultCtxLabel);
    tokens::setCaption(m_defaultCtxLabel);
    m_defaultCtxLabel->setProperty("indent", true);
    ctxLayout->addWidget(m_defaultCtxLabel);

    m_maxCtxLabel = new QLabel();
    tokens::setMuted(m_maxCtxLabel);
    tokens::setCaption(m_maxCtxLabel);
    m_maxCtxLabel->setProperty("indent", true);
    ctxLayout->addWidget(m_maxCtxLabel);

    form->addWidget(m_ctxGroup);

    m_sampleGroup = new QGroupBox();
    auto *sampleGrid = new QGridLayout(m_sampleGroup);

    m_tempCheck = new QCheckBox();
    sampleGrid->addWidget(m_tempCheck, 0, 0);
    m_tempSpin = new QDoubleSpinBox();
    m_tempSpin->setRange(0.0, 5.0);
    m_tempSpin->setSingleStep(0.05);
    m_tempSpin->setDecimals(2);
    m_tempSpin->setEnabled(false);
    sampleGrid->addWidget(m_tempSpin, 0, 1);

    m_topPCheck = new QCheckBox();
    sampleGrid->addWidget(m_topPCheck, 0, 2);
    m_topPSpin = new QDoubleSpinBox();
    m_topPSpin->setRange(0.0, 1.0);
    m_topPSpin->setSingleStep(0.05);
    m_topPSpin->setDecimals(2);
    m_topPSpin->setEnabled(false);
    sampleGrid->addWidget(m_topPSpin, 0, 3);

    m_topKCheck = new QCheckBox();
    sampleGrid->addWidget(m_topKCheck, 1, 0);
    m_topKSpin = new QSpinBox();
    m_topKSpin->setRange(1, 200);
    m_topKSpin->setEnabled(false);
    sampleGrid->addWidget(m_topKSpin, 1, 1);

    m_repeatPenaltyCheck = new QCheckBox();
    sampleGrid->addWidget(m_repeatPenaltyCheck, 1, 2);
    m_repeatPenaltySpin = new QDoubleSpinBox();
    m_repeatPenaltySpin->setRange(0.0, 2.0);
    m_repeatPenaltySpin->setSingleStep(0.05);
    m_repeatPenaltySpin->setDecimals(2);
    m_repeatPenaltySpin->setEnabled(false);
    sampleGrid->addWidget(m_repeatPenaltySpin, 1, 3);

    m_repeatLastNCheck = new QCheckBox();
    sampleGrid->addWidget(m_repeatLastNCheck, 2, 0);
    m_repeatLastNSpin = new QSpinBox();
    m_repeatLastNSpin->setRange(0, 2048);
    m_repeatLastNSpin->setEnabled(false);
    sampleGrid->addWidget(m_repeatLastNSpin, 2, 1);

    form->addWidget(m_sampleGroup);

    m_penaltyGroup = new QGroupBox();
    auto *penaltyGrid = new QGridLayout(m_penaltyGroup);

    m_presencePenaltyCheck = new QCheckBox();
    penaltyGrid->addWidget(m_presencePenaltyCheck, 0, 0);
    m_presencePenaltySpin = new QDoubleSpinBox();
    m_presencePenaltySpin->setRange(-2.0, 2.0);
    m_presencePenaltySpin->setSingleStep(0.05);
    m_presencePenaltySpin->setDecimals(2);
    m_presencePenaltySpin->setEnabled(false);
    penaltyGrid->addWidget(m_presencePenaltySpin, 0, 1);

    m_frequencyPenaltyCheck = new QCheckBox();
    penaltyGrid->addWidget(m_frequencyPenaltyCheck, 0, 2);
    m_frequencyPenaltySpin = new QDoubleSpinBox();
    m_frequencyPenaltySpin->setRange(-2.0, 2.0);
    m_frequencyPenaltySpin->setSingleStep(0.05);
    m_frequencyPenaltySpin->setDecimals(2);
    m_frequencyPenaltySpin->setEnabled(false);
    penaltyGrid->addWidget(m_frequencyPenaltySpin, 0, 3);

    form->addWidget(m_penaltyGroup);

    m_sysGroup = new QGroupBox();
    auto *sysLayout = new QVBoxLayout(m_sysGroup);

    auto *sysHeader = new QHBoxLayout();
    m_systemPromptCheck = new QCheckBox();
    sysHeader->addWidget(m_systemPromptCheck);
    sysHeader->addStretch();
    sysLayout->addLayout(sysHeader);

    m_systemPromptEdit = new QPlainTextEdit();
    m_systemPromptEdit->setMaximumHeight(180);
    m_systemPromptEdit->setEnabled(false);
    sysLayout->addWidget(m_systemPromptEdit);

    form->addWidget(m_sysGroup);

    form->addStretch();

    scrollArea->setWidget(scrollWidget);
    mainLayout->addWidget(scrollArea, 1);

    auto *btnLayout = new QHBoxLayout();
    m_previewBtn = new QPushButton();
    btnLayout->addWidget(m_previewBtn);

    m_createBtn = new QPushButton();
    m_createBtn->setProperty("accent", true);
    btnLayout->addWidget(m_createBtn);

    m_updateBtn = new QPushButton();
    m_updateBtn->setProperty("accent", true);
    m_updateBtn->setVisible(false);
    btnLayout->addWidget(m_updateBtn);

    m_clearBtn = new QPushButton();
    btnLayout->addWidget(m_clearBtn);

    mainLayout->addLayout(btnLayout);

    m_statusLabel = new QLabel();
    tokens::setMuted(m_statusLabel);
    tokens::setCaption(m_statusLabel);
    mainLayout->addWidget(m_statusLabel);

    connect(m_numCtxCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_numCtxSpin->setEnabled(checked);
        m_numCtxSlider->setEnabled(checked);
    });

    connect(m_numCtxSlider, &QSlider::valueChanged, this, [this](int v) {
        if (m_numCtxSpin->value() != v)
            m_numCtxSpin->setValue(v);
    });
    connect(m_numCtxSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) {
        if (m_numCtxSlider->value() != v)
            m_numCtxSlider->setValue(v);
    });

    auto connectCheckSpin = [this](QCheckBox *check, QWidget *spin) {
        connect(check, &QCheckBox::toggled, this, [spin](bool checked) {
            spin->setEnabled(checked);
        });
    };

    connectCheckSpin(m_tempCheck, m_tempSpin);
    connectCheckSpin(m_topPCheck, m_topPSpin);
    connectCheckSpin(m_topKCheck, m_topKSpin);
    connectCheckSpin(m_repeatPenaltyCheck, m_repeatPenaltySpin);
    connectCheckSpin(m_repeatLastNCheck, m_repeatLastNSpin);
    connectCheckSpin(m_presencePenaltyCheck, m_presencePenaltySpin);
    connectCheckSpin(m_frequencyPenaltyCheck, m_frequencyPenaltySpin);

    connect(m_systemPromptCheck, &QCheckBox::toggled, this, [this](bool checked) {
        m_systemPromptEdit->setEnabled(checked);
    });

    connect(m_fromCombo, &QComboBox::currentTextChanged, this, &ModelEditorWidget::onFromChanged);
    connect(m_fromCombo, QOverload<int>::of(&QComboBox::activated),
            this, &ModelEditorWidget::onFromActivated);
    connect(m_fromCombo, &QComboBox::customContextMenuRequested,
            this, &ModelEditorWidget::onFromContextMenu);
    connect(m_fromCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { updateDeleteQuantState(); });
    connect(m_deleteQuantBtn, &QToolButton::clicked, this, [this]() {
        const QString name = m_fromCombo->currentData().toString();
        if (!name.isEmpty())
            emit deleteQuantRequested(name);
    });
    connect(m_previewBtn, &QPushButton::clicked, this, &ModelEditorWidget::onPreview);
    connect(m_createBtn, &QPushButton::clicked, this, &ModelEditorWidget::onCreate);
    connect(m_updateBtn, &QPushButton::clicked, this, &ModelEditorWidget::onUpdate);
    connect(m_clearBtn, &QPushButton::clicked, this, &ModelEditorWidget::onClear);
    connect(m_numCtxSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ModelEditorWidget::onNumCtxChanged);

    retranslateUi();
    connect(I18n::instance(), &I18n::languageChanged, this, &ModelEditorWidget::retranslateUi);
}

void ModelEditorWidget::retranslateUi()
{
    m_titleLabel->setText(T("editor.title"));
    updateSourceLabel();
    m_unknownSourceLabel->setText(T("bind.unknownSource"));
    m_bindSourceBtn->setText(T("bind.open"));
    m_bindSourceBtn->setToolTip(T("bind.unknownSource"));

    m_modelGroup->setTitle(T("editor.modelGroup"));
    if (auto *l = qobject_cast<QLabel *>(m_modelLayout->labelForField(m_fromRowWidget)))
        l->setText(T("editor.from"));
    m_fromCombo->setToolTip(!m_repo.id.isEmpty() ? T("editor.sourceImmutableTip")
                                                 : T("editor.fromTip"));
    m_deleteQuantBtn->setToolTip(T("editor.deleteQuantTip"));
    m_deleteQuantBtn->setAccessibleName(T("editor.deleteQuant"));
    if (auto *l = qobject_cast<QLabel *>(m_modelLayout->labelForField(m_nameEdit)))
        l->setText(T("editor.name"));
    m_nameEdit->setPlaceholderText(T("editor.namePlaceholder"));
    m_nameEdit->setToolTip(T("editor.nameTip"));
    m_advancedBtn->setText(T("editor.advanced"));
    m_advancedBtn->setToolTip(T("editor.advancedTip"));
    m_modelInfoLabel->setToolTip(T("editor.infoTip"));

    if (auto *l = qobject_cast<QLabel *>(m_modelLayout->labelForField(m_rendererEdit)))
        l->setText(T("editor.renderer"));
    m_rendererEdit->setPlaceholderText(T("editor.directivePlaceholder"));
    m_rendererEdit->setToolTip(T("editor.rendererTip"));
    if (auto *l = qobject_cast<QLabel *>(m_modelLayout->labelForField(m_parserEdit)))
        l->setText(T("editor.parser"));
    m_parserEdit->setPlaceholderText(T("editor.directivePlaceholder"));
    m_parserEdit->setToolTip(T("editor.parserTip"));

    if (m_capabilities.isEmpty()) {
        m_capsLabel->clear();
        m_capsLabel->setVisible(false);
    } else {
        m_capsLabel->setText(T("editor.capabilities").arg(m_capabilities.join(", ")));
        m_capsLabel->setToolTip(T("editor.capsTip"));
        m_capsLabel->setVisible(true);
    }

    m_templateWarnLabel->setVisible(m_templateSoftSwitch);
    if (m_templateSoftSwitch)
        m_templateWarnLabel->setText(T("editor.templateSoftSwitch"));

    m_ctxGroup->setTitle(T("editor.ctxGroup"));
    m_numCtxCheck->setText(T("editor.numCtx"));
    m_numCtxCheck->setToolTip(T("editor.numCtxTip"));
    m_numCtxSlider->setToolTip(T("editor.sliderTip"));
    m_numCtxSpin->setToolTip(T("editor.spinTip"));
    m_ctxUnitLabel->setText(T("editor.kUnit"));

    m_sampleGroup->setTitle(T("editor.sampleGroup"));
    m_tempCheck->setText(T("editor.temp"));
    m_tempSpin->setToolTip(T("editor.tempTip"));
    m_topPCheck->setText(T("editor.topP"));
    m_topPSpin->setToolTip(T("editor.topPTip"));
    m_topKCheck->setText(T("editor.topK"));
    m_topKSpin->setToolTip(T("editor.topKTip"));
    m_repeatPenaltyCheck->setText(T("editor.repeatPenalty"));
    m_repeatPenaltySpin->setToolTip(T("editor.repeatPenaltyTip"));
    m_repeatLastNCheck->setText(T("editor.repeatLastN"));
    m_repeatLastNSpin->setToolTip(T("editor.repeatLastNTip"));

    m_penaltyGroup->setTitle(T("editor.penaltyGroup"));
    m_presencePenaltyCheck->setText(T("editor.presencePenalty"));
    m_presencePenaltySpin->setToolTip(T("editor.presencePenaltyTip"));
    m_frequencyPenaltyCheck->setText(T("editor.frequencyPenalty"));
    m_frequencyPenaltySpin->setToolTip(T("editor.frequencyPenaltyTip"));

    m_sysGroup->setTitle(T("editor.sysGroup"));
    m_systemPromptCheck->setText(T("editor.sysEnable"));
    m_systemPromptEdit->setPlaceholderText(T("editor.sysPlaceholder"));

    updateModelButtons();
    m_previewBtn->setText(T("editor.preview"));
    m_previewBtn->setToolTip(T("editor.previewTip"));
    m_createBtn->setText(T("editor.create"));
    m_createBtn->setToolTip(T("editor.createTip"));
    m_updateBtn->setText(T("editor.update"));
    m_updateBtn->setToolTip(T("editor.updateTip"));
    m_clearBtn->setText(T("editor.clear"));
    m_clearBtn->setToolTip(T("editor.clearTip"));
}

void ModelEditorWidget::updateCapabilities(const OllamaCli::ModelParams &params)
{
    m_capabilities = params.capabilities;
    if (m_capabilities.isEmpty()) {
        m_capsLabel->clear();
        m_capsLabel->setVisible(false);
        return;
    }
    m_capsLabel->setText(T("editor.capabilities").arg(m_capabilities.join(", ")));
    m_capsLabel->setToolTip(T("editor.capsTip"));
    m_capsLabel->setVisible(true);
}

void ModelEditorWidget::updateTemplateInfo(const OllamaCli::ModelParams &params)
{
    const QString &t = params.templateStr;
    m_templateSoftSwitch = t.contains(QLatin1String("IsThinkSet"))
            || t.contains(QLatin1String("/no_think"))
            || t.contains(QLatin1String("/think"));
    m_templateWarnLabel->setVisible(m_templateSoftSwitch);
    if (m_templateSoftSwitch)
        m_templateWarnLabel->setText(T("editor.templateSoftSwitch"));
}

void ModelEditorWidget::applyFromComboColors()
{
    const theme::Palette &p = tokens::activePalette();
    for (int i = 0; i < m_fromCombo->count(); ++i) {
        if (m_fromCombo->itemData(i).toString().isEmpty())
            continue; // separator
        if (m_fromCombo->itemData(i, Qt::UserRole + 1).toBool())
            continue; // already installed: default text colour
        const int fit = m_fromCombo->itemData(i, Qt::UserRole + 2).toInt();
        const bool warning = fit == int(memoryinfo::Fit::No)
                || fit == int(memoryinfo::Fit::Tight);
        m_fromCombo->setItemData(i, warning ? p.warning : p.muted, Qt::ForegroundRole);
    }
}

void ModelEditorWidget::rebuildFromCombo(const QString &select)
{
    // `select` is always a model name (the item's data), never the visible
    // label: an installed quant is labelled with its original ref, so the two
    // differ whenever the model was renamed.
    const QString current = select.isEmpty()
            ? (m_fromCombo->currentData().isValid() ? m_fromCombo->currentData().toString()
                                                    : m_fromCombo->currentText().trimmed())
            : select;

    m_fromCombo->blockSignals(true);
    m_fromCombo->clear();

    if (!m_repo.id.isEmpty()) {
        // FROM of an installed repository: only its own installed quants.
        for (const InstalledQuant &quant : std::as_const(m_repo.quants)) {
            // Ollama quants are identified by their original tag; a HuggingFace
            // quant by the file's quantization label (IQ4_XS-MTP), because its
            // ref is the repository, not the variant.
            QString label = quant.originalLabel;
            if (label.isEmpty())
                label = quant.originalRef.isEmpty() ? quant.modelName : quant.originalRef;
            if (quant.renamed())
                label += QStringLiteral("   · ") + quant.modelName;
            m_fromCombo->addItem(label, quant.modelName);
            const int row = m_fromCombo->count() - 1;
            m_fromCombo->setItemData(row, true, Qt::UserRole + 1);
            m_fromCombo->setItemData(row, true, Qt::UserRole + 2);
        }
    } else {
        for (const QString &name : std::as_const(m_localModels))
            m_fromCombo->addItem(name, name);
    }

    if (m_repo.id.isEmpty() && !m_familyTags.isEmpty()) {
        if (m_fromCombo->count() > 0)
            m_fromCombo->insertSeparator(m_fromCombo->count());
        for (const RegistryClient::Tag &tag : std::as_const(m_familyTags)) {
            // Already listed as an installed quant of this repository.
            bool duplicate = false;
            for (const InstalledQuant &quant : std::as_const(m_repo.quants)) {
                if (quant.modelName == tag.name)
                    duplicate = true;
            }
            if (duplicate)
                continue;
            if (m_repo.id.isEmpty()) {
                // Only quants of this very model line, not every tag of the family
                // (qwen3 alone has 90+ tags from 0.6b to 235b).
                const QString tagStem = stripQuantSuffix(tag.name.section(QLatin1Char(':'), 1));
                if (!m_modelStem.isEmpty() && tagStem != m_modelStem)
                    continue;
            }
            const bool installed = m_localModels.contains(tag.name);
            const quint64 bytes = memoryinfo::parseSizeBytes(tag.size);
            const memoryinfo::Fit fit = memoryinfo::fitLevel(bytes);
            QString label = tag.name;
            if (installed)
                label += QStringLiteral("   \u2713 ") + T("editor.quantInstalledMark");
            else
                label += QStringLiteral("   \u2193 ") + tag.size;
            if (fit == memoryinfo::Fit::No)
                label += QStringLiteral("   · ") + T("browser.noFit");
            else if (fit == memoryinfo::Fit::Tight)
                label += QStringLiteral("   · ") + T("browser.mayNotFit");
            m_fromCombo->addItem(dotIcon(fit), label, tag.name);
            const int row = m_fromCombo->count() - 1;
            // Keep the flags, not the colours: the colours are re-derived from
            // the active palette whenever the theme changes.
            m_fromCombo->setItemData(row, installed, Qt::UserRole + 1);
            m_fromCombo->setItemData(row, int(fit), Qt::UserRole + 2);
        }
    }
    m_fromCombo->blockSignals(false);

    applyFromComboColors();

    int idx = m_fromCombo->findData(current);
    if (idx < 0)
        idx = m_fromCombo->findText(current);
    if (idx >= 0)
        m_fromCombo->setCurrentIndex(idx);
    updateModelButtons();
}

void ModelEditorWidget::requestFamilyTags()
{
    if (m_family.isEmpty())
        return;
    m_statusLabel->setText(T("editor.loadingQuants").arg(m_family));
    m_registry->listTags(m_family);
}

void ModelEditorWidget::onFamilyTags(bool ok, const QList<RegistryClient::Tag> &tags,
                                     const QString &error)
{
    if (!ok) {
        m_statusLabel->setText(T("editor.quantsFailed").arg(error));
        return;
    }
    m_familyTags = tags;
    rebuildFromCombo(currentFromName());
    m_statusLabel->setText(T("editor.quantsLoaded").arg(tags.size()));
}

bool ModelEditorWidget::selectedFromInstalled() const
{
    return m_localModels.contains(currentFromName());
}

QString ModelEditorWidget::currentFromName() const
{
    const QString modelName = m_fromCombo->currentData().toString();
    return modelName.isEmpty() ? m_fromCombo->currentText().trimmed() : modelName;
}

void ModelEditorWidget::updateNameForSelection()
{
    if (m_repo.id.isEmpty())
        return;

    const QString name = currentFromName();
    const bool installed = !name.isEmpty() && m_localModels.contains(name);
    m_nameEdit->setEnabled(installed);
    if (installed) {
        m_nameEdit->setPlaceholderText(T("editor.namePlaceholder"));
        m_nameEdit->setText(name);
    } else {
        m_nameEdit->clear();
        m_nameEdit->setPlaceholderText(T("editor.notInstalled"));
    }
    updateModelButtons();
}

void ModelEditorWidget::updateSourceLabel()
{
    if (m_repo.id.isEmpty()) {
        m_sourceLabel->clear();
        m_sourceLabel->setVisible(false);
        m_unknownSourceLabel->setVisible(false);
        m_bindSourceBtn->setVisible(false);
        return;
    }

    QString text = m_repo.local
            ? T("list.localSource")
            : (m_repo.owner.isEmpty() ? m_repo.title
                                      : T("editor.sourceLine").arg(m_repo.title, m_repo.owner));
    m_sourceLabel->setText(text);
    m_sourceLabel->setToolTip(m_repo.ref.isEmpty()
            ? T("editor.sourceImmutableTip")
            : T("editor.sourceImmutableTip") + QLatin1Char('\n') + m_repo.ref);
    m_sourceLabel->setVisible(true);

    // A local card has no origin at all: say so and offer the repair path.
    m_unknownSourceLabel->setVisible(m_repo.local);
    m_bindSourceBtn->setVisible(m_repo.local);
}

void ModelEditorWidget::applyRepoModeToFrom()
{
    const bool repoMode = !m_repo.id.isEmpty();
    m_fromCombo->setEditable(!repoMode);
    // A local model has nothing to choose from: its single quant is the source.
    m_fromCombo->setEnabled(!(repoMode && m_repo.local && m_repo.quants.size() <= 1));
    m_fromCombo->setToolTip(repoMode ? T("editor.sourceImmutableTip") : T("editor.fromTip"));
}

void ModelEditorWidget::updateDeleteQuantState()
{
    const QString name = m_fromCombo->currentData().toString();
    bool installed = false;
    if (!name.isEmpty()) {
        if (!m_repo.id.isEmpty()) {
            for (const InstalledQuant &quant : std::as_const(m_repo.quants)) {
                if (quant.modelName == name) {
                    installed = true;
                    break;
                }
            }
        } else {
            installed = m_localModels.contains(name)
                    || (!m_currentEditModel.isEmpty() && name == m_currentEditModel);
        }
    }
    m_deleteQuantBtn->setEnabled(installed);
}

void ModelEditorWidget::updateModelButtons()
{
    updateDeleteQuantState();
    const bool installed = selectedFromInstalled();
    const bool creating = m_currentEditModel.isEmpty();

    QPushButton *primary = creating ? m_createBtn : m_updateBtn;
    if (installed) {
        primary->setText(creating ? T("editor.create") : T("editor.update"));
        primary->setToolTip(creating ? T("editor.createTip") : T("editor.updateTip"));
    } else {
        primary->setText(T("editor.installQuant"));
        primary->setToolTip(T("editor.installQuantTip"));
    }
}

void ModelEditorWidget::installSelectedQuant()
{
    const QString name = m_fromCombo->currentData().toString();
    if (name.isEmpty() || m_localModels.contains(name))
        return;
    if (m_installJob)
        return;
    // One `ollama pull` at a time: while a discover tab downloads, ours waits
    // in the CLI queue instead of killing that download (B-2).
    m_fromCombo->setEnabled(false);
    m_statusLabel->setText(m_cli->isPulling() ? T("editor.quantQueued").arg(name)
                                              : T("editor.quantPulling").arg(name));
    OllamaJob *job = m_cli->pullModelAsync(name);
    m_installJob = job;
    connect(job, &OllamaJob::finished, this, [this, name](bool ok, const QString &output) {
        m_installJob.clear();
        applyRepoModeToFrom();
        if (!ok) {
            m_statusLabel->setText(T("editor.quantFailed").arg(output));
            return;
        }
        m_statusLabel->setText(T("editor.quantInstalled").arg(name));
        m_localModels.append(name);
        rebuildFromCombo(name);
        updateNameForSelection();
        updateModelButtons();
    });
}

void ModelEditorWidget::onFromActivated(int index)
{
    if (index < 0)
        return;

    const QString name = m_fromCombo->currentData().toString();
    if (!m_repo.id.isEmpty() && !name.isEmpty() && m_localModels.contains(name)
            && name != m_currentEditModel) {
        // Another installed quant of this repository: show its parameters.
        loadModel(name);
        return;
    }

    updateNameForSelection();
    updateModelButtons();
    if (!selectedFromInstalled())
        m_statusLabel->setText(T("editor.quantNotInstalled"));
}

void ModelEditorWidget::onFromContextMenu(QPoint pos)
{
    const int index = m_fromCombo->view()->currentIndex().row();
    if (index < 0)
        return;
    const QString name = m_fromCombo->itemData(index).toString();
    if (name.isEmpty() || !m_localModels.contains(name))
        return;

    QMenu menu(this);
    QAction *removeAction = menu.addAction(T("editor.removeQuant"));
    if (menu.exec(m_fromCombo->mapToGlobal(pos)) != removeAction)
        return;

    if (QMessageBox::question(this, T("editor.removeQuantTitle"),
                              T("editor.removeQuantConfirm").arg(name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
            == QMessageBox::Yes) {
        // Through the service: the quant's provenance and leftover downloads
        // go with it, bound to this very job (B-11).
        m_install->deleteModel(name);
    }
}

void ModelEditorWidget::setAvailableModels(const QStringList &models)
{
    m_localModels = models;
    rebuildFromCombo(currentFromName());
    updateNameForSelection();
}

void ModelEditorWidget::setCreateMode()
{
    m_repo = RepoGroup();
    m_currentEditModel.clear();
    m_family.clear();
    m_familyTags.clear();
    m_modelStem.clear();
    rebuildFromCombo();
    applyRepoModeToFrom();
    updateSourceLabel();
    m_loadingModel.clear();
    m_loadJob.clear();
    m_createBtn->setVisible(true);
    m_updateBtn->setVisible(false);
    m_nameEdit->setReadOnly(false);
    m_nameEdit->setEnabled(true);
    m_nameEdit->setPlaceholderText(T("editor.namePlaceholder"));
    m_nameEdit->clear();
    m_statusLabel->setText(T("editor.statusCreate"));

    m_numCtxCheck->setChecked(true);
    m_numCtxSpin->setValue(32);

    m_tempCheck->setChecked(false);
    m_tempSpin->setValue(0.80);
    m_topPCheck->setChecked(false);
    m_topPSpin->setValue(0.90);
    m_topKCheck->setChecked(false);
    m_topKSpin->setValue(40);
    m_repeatPenaltyCheck->setChecked(false);
    m_repeatPenaltySpin->setValue(1.10);
    m_repeatLastNCheck->setChecked(false);
    m_repeatLastNSpin->setValue(64);
    m_presencePenaltyCheck->setChecked(false);
    m_presencePenaltySpin->setValue(0.0);
    m_frequencyPenaltyCheck->setChecked(false);
    m_frequencyPenaltySpin->setValue(0.0);

    m_systemPromptCheck->setChecked(false);
    m_systemPromptEdit->clear();

    m_rendererEdit->clear();
    m_parserEdit->clear();
    m_capabilities.clear();
    m_capsLabel->clear();
    m_capsLabel->setVisible(false);
    m_templateSoftSwitch = false;
    m_templateWarnLabel->setVisible(false);

    m_modelInfoLabel->clear();
    updateModelRowVisibility();
    m_defaultCtxLabel->clear();
    m_maxCtxLabel->clear();
    m_baseMaxContext = 32768;
    m_numCtxSlider->setMaximum(1024);
    m_numCtxSpin->setMaximum(1024);
}

void ModelEditorWidget::setEditMode(const QString &modelName)
{
    m_currentEditModel = modelName;
    updateModelRowVisibility();
    m_createBtn->setVisible(false);
    m_updateBtn->setVisible(true);
    m_nameEdit->setEnabled(true);
    m_nameEdit->setPlaceholderText(T("editor.namePlaceholder"));
    m_nameEdit->setText(modelName);
    m_statusLabel->setText(T("editor.statusEdit").arg(modelName));
}

void ModelEditorWidget::onClear()
{
    // "Clear" on an installed repository goes back to its first quant instead
    // of leaving the user on an empty screen.
    if (!m_repo.id.isEmpty()) {
        RepoGroup group = m_repo;
        m_currentEditModel.clear();
        loadRepo(group);
        return;
    }
    setCreateMode();
}

void ModelEditorWidget::loadRepo(const RepoGroup &group)
{
    if (group.quants.isEmpty()) {
        setCreateMode();
        return;
    }

    m_repo = group;
    m_familyTags.clear();
    // Only the Ollama registry can supply extra quants; an HF repo only ever
    // shows the quants that are already installed.
    m_family = group.source == QLatin1String("ollama")
            ? group.ref.section(QLatin1Char(':'), 0, 0)
            : QString();
    m_modelStem.clear();

    updateSourceLabel();
    applyRepoModeToFrom();

    QString target = m_currentEditModel;
    bool known = false;
    for (const InstalledQuant &quant : group.quants) {
        if (quant.modelName == target) {
            known = true;
            break;
        }
    }
    if (!known)
        target = group.quants.first().modelName;

    rebuildFromCombo(target);
    loadModel(target);
}

void ModelEditorWidget::loadModel(const QString &modelName)
{
    // `ollama show` takes a round trip to the daemon (and may hang with it):
    // never on the UI thread. The form keeps the previous model until the
    // reply arrives, and m_currentEditModel still names that model.
    m_loadingModel = modelName;
    m_statusLabel->setText(T("editor.statusLoading").arg(modelName));
    OllamaJob *job = m_cli->showModelAsync(modelName);
    m_loadJob = job;
    connect(job, &OllamaJob::finished, this, [this, job, modelName](bool ok, const QString &) {
        // Only the latest selection is applied; create mode drops it.
        if (job != m_loadJob)
            return;
        m_loadJob.clear();
        m_loadingModel.clear();
        applyLoadedModel(modelName, ok ? job->params() : OllamaCli::ModelParams());
    });
}

void ModelEditorWidget::applyLoadedModel(const QString &modelName,
                                         const OllamaCli::ModelParams &params)
{
    if (params.architecture.isEmpty()) {
        m_statusLabel->setText(T("editor.statusLoadError").arg(modelName));
        m_defaultCtxLabel->clear();
        m_maxCtxLabel->clear();
        return;
    }

    m_fromCombo->blockSignals(true);
    const QString baseFrom = modelName.isEmpty() ? params.fromBase : modelName;
    if (!baseFrom.isEmpty()) {
        int idx = m_fromCombo->findData(baseFrom);
        if (idx < 0)
            idx = m_fromCombo->findText(baseFrom);
        if (idx >= 0)
            m_fromCombo->setCurrentIndex(idx);
        else if (m_fromCombo->isEditable())
            m_fromCombo->setCurrentText(baseFrom);
    }
    m_fromCombo->blockSignals(false);

    int maxK = (params.contextLength > 0) ? params.contextLength / 1024 : 1024;
    m_baseMaxContext = (params.contextLength > 0) ? params.contextLength : 32768;
    m_numCtxSlider->setMaximum(maxK);
    m_numCtxSpin->setMaximum(maxK);
    setMaxCtxLabel(params.contextLength);

    m_numCtxCheck->setChecked(params.parameters.contains("num_ctx"));
    if (params.parameters.contains("num_ctx")) {
        int wantK = qRound(params.parameters.value("num_ctx").toInt() / 1024.0);
        m_numCtxSpin->setValue(wantK);
        tokens::setRole(m_numCtxSpin, wantK > maxK ? "danger" : "");
    }

    // Absent parameters use Ollama's own defaults; show those instead of the
    // spin box's zero, so an unchecked field never looks like "0.0".
    m_tempCheck->setChecked(params.parameters.contains("temperature"));
    m_tempSpin->setValue(params.parameters.contains("temperature")
            ? params.parameters.value("temperature").toDouble() : 0.80);

    m_topPCheck->setChecked(params.parameters.contains("top_p"));
    m_topPSpin->setValue(params.parameters.contains("top_p")
            ? params.parameters.value("top_p").toDouble() : 0.90);

    m_topKCheck->setChecked(params.parameters.contains("top_k"));
    m_topKSpin->setValue(params.parameters.contains("top_k")
            ? params.parameters.value("top_k").toInt() : 40);

    m_repeatPenaltyCheck->setChecked(params.parameters.contains("repeat_penalty"));
    if (params.parameters.contains("repeat_penalty"))
        m_repeatPenaltySpin->setValue(params.parameters.value("repeat_penalty").toDouble());

    m_repeatLastNCheck->setChecked(params.parameters.contains("repeat_last_n"));
    if (params.parameters.contains("repeat_last_n"))
        m_repeatLastNSpin->setValue(params.parameters.value("repeat_last_n").toInt());

    m_presencePenaltyCheck->setChecked(params.parameters.contains("presence_penalty"));
    if (params.parameters.contains("presence_penalty"))
        m_presencePenaltySpin->setValue(params.parameters.value("presence_penalty").toDouble());

    m_frequencyPenaltyCheck->setChecked(params.parameters.contains("frequency_penalty"));
    if (params.parameters.contains("frequency_penalty"))
        m_frequencyPenaltySpin->setValue(params.parameters.value("frequency_penalty").toDouble());

    bool hasSystem = !params.systemPrompt.isEmpty();
    m_systemPromptCheck->setChecked(hasSystem);
    if (hasSystem)
        m_systemPromptEdit->setPlainText(params.systemPrompt);

    if (m_repo.id.isEmpty()) {
        m_family = modelName.section(QLatin1Char(':'), 0, 0);
        m_modelStem = stripQuantSuffix(modelName.section(QLatin1Char(':'), 1));
    }
    setEditMode(modelName);
    // The editor lists installed quants only, for both sources. Downloading
    // another quant belongs to the discover tabs, where it has a proper state.
    if (m_repo.id.isEmpty())
        requestFamilyTags();
    else
        m_statusLabel->setText(T("editor.quantsInstalled").arg(m_repo.quants.size()));

    m_rendererEdit->setText(params.renderer);
    m_parserEdit->setText(params.parser);
    updateCapabilities(params);
    updateTemplateInfo(params);

    QStringList info;
    const QString base = params.fromBase;
    if (!base.isEmpty() && base != modelName)
        info << T("editor.modelInfoBase").arg(base);
    if (!params.renderer.isEmpty() || !params.parser.isEmpty()) {
        info << T("editor.modelInfoDirectives")
                .arg(params.renderer.isEmpty() ? QStringLiteral("—") : params.renderer,
                     params.parser.isEmpty() ? QStringLiteral("—") : params.parser);
    }
    const quint64 available = memoryinfo::availableBytes();
    if (available > 0) {
        info << T("editor.memoryAvailable")
                .arg(QString::number(double(available) / (1024.0 * 1024.0 * 1024.0), 'f', 0));
    }
    m_modelInfoLabel->setText(info.join(QStringLiteral("  ·  ")));

    const QString from = currentFromName();
    if (from.isEmpty() || from == modelName)
        applyContextInfo(params, params.contextLength);
    else
        requestBaseContext(from);

    m_statusLabel->setText(T("editor.statusLoaded")
        .arg(modelName, params.architecture, params.paramCount));
}

void ModelEditorWidget::onFromChanged(const QString &baseModel)
{
    // The parameter is the visible text; the model name lives in the item data
    // (they differ for renamed quants and for the size hints of registry tags).
    const QString base = currentFromName();
    const QString label = base.isEmpty() ? baseModel : base;
    if (label.isEmpty()) {
        m_defaultCtxLabel->clear();
        m_maxCtxLabel->clear();
        m_baseMaxContext = 32768;
        m_numCtxSlider->setMaximum(1024);
        m_numCtxSpin->setMaximum(1024);
        m_ctxBase.clear();
        m_ctxJob.clear();
        m_ctxTimer->stop();
        return;
    }

    requestBaseContext(label);
}

void ModelEditorWidget::requestBaseContext(const QString &baseModel)
{
    m_ctxBase = baseModel;
    m_ctxTimer->start();
}

void ModelEditorWidget::applyBaseContext(bool ok, const OllamaCli::ModelParams &params)
{
    const int maxCtx = ok ? params.contextLength : 0;
    if (maxCtx > 0) {
        m_baseMaxContext = maxCtx;
        m_numCtxSlider->setMaximum(maxCtx / 1024);
        m_numCtxSpin->setMaximum(maxCtx / 1024);
        setMaxCtxLabel(maxCtx);
        if (m_numCtxSpin->value() > maxCtx / 1024)
            m_numCtxSpin->setValue(maxCtx / 1024);
    } else {
        m_maxCtxLabel->clear();
        m_baseMaxContext = 32768;
        m_numCtxSlider->setMaximum(1024);
        m_numCtxSpin->setMaximum(1024);
    }

    applyContextInfo(ok ? params : OllamaCli::ModelParams(), maxCtx);
}

void ModelEditorWidget::onNumCtxChanged(int value)
{
    tokens::setRole(m_numCtxSpin,
                    (m_baseMaxContext > 0 && value * 1024 > m_baseMaxContext) ? "danger" : "");
}

void ModelEditorWidget::onPreview()
{
    auto config = collectConfig();
    if (config.from.isEmpty()) {
        QMessageBox::warning(this, T("editor.warnTitle"), T("editor.warnFrom"));
        return;
    }
    QString content;
    if (!buildModelfile(config, &content))
        return;
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(T("editor.previewTitle"));
    msgBox.setText(content);
    msgBox.setDetailedText(content);
    msgBox.exec();
}

void ModelEditorWidget::onCreate()
{
    if (!selectedFromInstalled() && !m_familyTags.isEmpty()
            && !m_fromCombo->currentData().toString().isEmpty()) {
        installSelectedQuant();
        return;
    }
    auto config = collectConfig();
    if (config.from.isEmpty()) {
        QMessageBox::warning(this, T("editor.warnTitle"), T("editor.warnFrom"));
        return;
    }
    QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, T("editor.warnTitle"), T("editor.warnName"));
        return;
    }
    QString content;
    if (!buildModelfile(config, &content))
        return;
    emit createRequested(name, content);
}

void ModelEditorWidget::onUpdate()
{
    if (m_currentEditModel.isEmpty())
        return;
    if (!selectedFromInstalled()) {
        installSelectedQuant();
        return;
    }

    auto config = collectConfig();
    if (config.from.isEmpty()) {
        QMessageBox::warning(this, T("editor.warnTitle"), T("editor.warnFrom"));
        return;
    }
    const QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, T("editor.warnTitle"), T("editor.warnName"));
        return;
    }

    // Editing the name of an installed model means renaming, not creating a
    // second copy with a different name. The parameters must follow the new
    // name, so the two are sent together.
    if (name != m_currentEditModel) {
        if (m_localModels.contains(name)) {
            QMessageBox::warning(this, T("editor.warnTitle"), T("editor.nameConflict"));
            return;
        }
        // After the rename the old name is gone: the Modelfile must use the new
        // one, otherwise `ollama create` would have nothing to inherit from.
        config.from = name;
        QString content;
        if (!buildModelfile(config, &content))
            return;
        emit renameAndUpdateRequested(m_currentEditModel, name, content);
        return;
    }

    QString content;
    if (!buildModelfile(config, &content))
        return;
    emit updateRequested(name, content);
}

bool ModelEditorWidget::buildModelfile(const ModelfileBuilder::Config &config, QString *content)
{
    QString directive;
    *content = ModelfileBuilder::build(config, &directive);
    if (!directive.isEmpty()) {
        QMessageBox::warning(this, T("editor.warnTitle"), T("editor.tripleQuotes").arg(directive));
        return false;
    }
    return true;
}

ModelfileBuilder::Config ModelEditorWidget::collectConfig()
{
    ModelfileBuilder::Config config;
    config.from = currentFromName();

    config.enableNumCtx = m_numCtxCheck->isChecked();
    config.numCtx = m_numCtxSpin->value() * 1024;

    config.enableTemperature = m_tempCheck->isChecked();
    config.temperature = m_tempSpin->value();

    config.enableTopP = m_topPCheck->isChecked();
    config.topP = m_topPSpin->value();

    config.enableTopK = m_topKCheck->isChecked();
    config.topK = m_topKSpin->value();

    config.enableRepeatPenalty = m_repeatPenaltyCheck->isChecked();
    config.repeatPenalty = m_repeatPenaltySpin->value();

    config.enableRepeatLastN = m_repeatLastNCheck->isChecked();
    config.repeatLastN = m_repeatLastNSpin->value();

    config.enablePresencePenalty = m_presencePenaltyCheck->isChecked();
    config.presencePenalty = m_presencePenaltySpin->value();

    config.enableFrequencyPenalty = m_frequencyPenaltyCheck->isChecked();
    config.frequencyPenalty = m_frequencyPenaltySpin->value();

    config.enableSystemPrompt = m_systemPromptCheck->isChecked();
    config.systemPrompt = m_systemPromptEdit->toPlainText().trimmed();

    config.renderer = m_rendererEdit->text().trimmed();
    config.parser = m_parserEdit->text().trimmed();

    return config;
}

QString ModelEditorWidget::getModelName() const
{
    return m_nameEdit->text().trimmed();
}

void ModelEditorWidget::clearForm()
{
    setCreateMode();
    m_fromCombo->setCurrentIndex(-1);
    m_defaultCtxLabel->clear();
    m_maxCtxLabel->clear();
    m_systemPromptEdit->clear();
}

void ModelEditorWidget::applyContextInfo(const OllamaCli::ModelParams &params,
                                         int loadedContextLength)
{
    int defaultCtx = 2048;
    if (params.parameters.contains("num_ctx")) {
        defaultCtx = params.parameters.value("num_ctx").toInt();
    }
    setDefaultCtxLabel(defaultCtx);

    int maxCtx = params.contextLength;
    if (maxCtx <= 0 && loadedContextLength > 0)
        maxCtx = loadedContextLength;
    if (maxCtx > 0)
        setMaxCtxLabel(maxCtx);
}

void ModelEditorWidget::setDefaultCtxLabel(int defaultCtx)
{
    if (defaultCtx > 0) {
        m_defaultCtxLabel->setText(T("editor.defaultCtx")
            .arg(defaultCtx).arg(defaultCtx / 1024));
    } else {
        m_defaultCtxLabel->clear();
    }
}

void ModelEditorWidget::setMaxCtxLabel(int maxCtx)
{
    if (maxCtx > 0) {
        m_maxCtxLabel->setText(T("editor.maxCtx")
            .arg(maxCtx).arg(maxCtx / 1024));
    } else {
        m_maxCtxLabel->clear();
    }
}

void ModelEditorWidget::setAdvancedVisible(bool visible)
{
    // Rows: 0 = FROM, 1 = name, 2 = renderer, 3 = parser.
    // FROM is the main field now (it lists the quants of the repository), so it
    // is never hidden; "Advanced" only reveals the technical directives.
    m_modelLayout->setRowVisible(0, true);
    m_modelLayout->setRowVisible(1, true);
    m_modelLayout->setRowVisible(2, visible);
    m_modelLayout->setRowVisible(3, visible);
}

void ModelEditorWidget::updateModelRowVisibility()
{
    // Collapsed by default for both modes: an installed model shows only its
    // name, a new model additionally shows FROM.
    m_advancedBtn->setChecked(false);
    setAdvancedVisible(false);
}

void ModelEditorWidget::toggleParamEnabled(QCheckBox *check, QWidget *widget)
{
    widget->setEnabled(check->isChecked());
}
