#include "modeleditorwidget.h"
#include <QScrollArea>
#include <QMessageBox>
#include <QApplication>
#include <QStyle>
#include <QGridLayout>
#include <QRegularExpressionValidator>

ModelEditorWidget::ModelEditorWidget(OllamaCli *cli, QWidget *parent)
    : QWidget(parent), m_cli(cli)
{
    setupUI();
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

    m_fromCombo = new QComboBox();
    m_fromCombo->setEditable(true);
    m_fromCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_fromCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_modelLayout->addRow(new QLabel(), m_fromCombo);

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

    form->addWidget(m_modelGroup);

    m_capsLabel = new QLabel();
    m_capsLabel->setStyleSheet("color: #666; font-size: 11px; padding-left: 4px;");
    m_capsLabel->setWordWrap(true);
    form->addWidget(m_capsLabel);

    m_templateWarnLabel = new QLabel();
    m_templateWarnLabel->setWordWrap(true);
    m_templateWarnLabel->setStyleSheet("color: #b36b00; font-size: 11px; padding-left: 4px;");
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
    m_defaultCtxLabel->setStyleSheet("color: #666; font-size: 11px; padding-left: 4px;");
    ctxLayout->addWidget(m_defaultCtxLabel);

    m_maxCtxLabel = new QLabel();
    m_maxCtxLabel->setStyleSheet("color: #666; font-size: 11px; padding-left: 4px;");
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
    m_createBtn->setStyleSheet("QPushButton { font-weight: bold; }");
    btnLayout->addWidget(m_createBtn);

    m_updateBtn = new QPushButton();
    m_updateBtn->setVisible(false);
    btnLayout->addWidget(m_updateBtn);

    m_clearBtn = new QPushButton();
    btnLayout->addWidget(m_clearBtn);

    mainLayout->addLayout(btnLayout);

    m_statusLabel = new QLabel();
    m_statusLabel->setStyleSheet("color: #888; font-size: 11px; padding: 2px;");
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
    connect(m_previewBtn, &QPushButton::clicked, this, &ModelEditorWidget::onPreview);
    connect(m_createBtn, &QPushButton::clicked, this, &ModelEditorWidget::onCreate);
    connect(m_updateBtn, &QPushButton::clicked, this, &ModelEditorWidget::onUpdate);
    connect(m_clearBtn, &QPushButton::clicked, this, &ModelEditorWidget::setCreateMode);
    connect(m_numCtxSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &ModelEditorWidget::onNumCtxChanged);

    retranslateUi();
    connect(I18n::instance(), &I18n::languageChanged, this, &ModelEditorWidget::retranslateUi);
}

void ModelEditorWidget::retranslateUi()
{
    m_titleLabel->setText(T("editor.title"));

    m_modelGroup->setTitle(T("editor.modelGroup"));
    if (auto *l = qobject_cast<QLabel *>(m_modelLayout->labelForField(m_fromCombo)))
        l->setText(T("editor.from"));
    m_fromCombo->setToolTip(T("editor.fromTip"));
    if (auto *l = qobject_cast<QLabel *>(m_modelLayout->labelForField(m_nameEdit)))
        l->setText(T("editor.name"));
    m_nameEdit->setPlaceholderText(T("editor.namePlaceholder"));
    m_nameEdit->setToolTip(T("editor.nameTip"));

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

void ModelEditorWidget::setAvailableModels(const QStringList &models){
    QString current = m_fromCombo->currentText();
    m_fromCombo->blockSignals(true);
    m_fromCombo->clear();
    m_fromCombo->addItems(models);
    if (!current.isEmpty()) {
        int idx = m_fromCombo->findText(current);
        if (idx >= 0) m_fromCombo->setCurrentIndex(idx);
    }
    m_fromCombo->blockSignals(false);
}

void ModelEditorWidget::setCreateMode()
{
    m_currentEditModel.clear();
    m_fromBlob.clear();
    m_createBtn->setVisible(true);
    m_updateBtn->setVisible(false);
    m_nameEdit->setReadOnly(false);
    m_nameEdit->clear();
    m_statusLabel->setText(T("editor.statusCreate"));

    m_numCtxCheck->setChecked(true);
    m_numCtxSpin->setValue(32);

    m_tempCheck->setChecked(false);
    m_tempSpin->setValue(0.70);
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

    m_defaultCtxLabel->clear();
    m_maxCtxLabel->clear();
    m_baseMaxContext = 32768;
    m_numCtxSlider->setMaximum(1024);
    m_numCtxSpin->setMaximum(1024);
}

void ModelEditorWidget::setEditMode(const QString &modelName)
{
    m_currentEditModel = modelName;
    m_createBtn->setVisible(false);
    m_updateBtn->setVisible(true);
    m_nameEdit->setText(modelName);
    m_statusLabel->setText(T("editor.statusEdit").arg(modelName));
}

void ModelEditorWidget::loadModel(const QString &modelName)
{
    OllamaCli::ModelParams params = m_cli->showModel(modelName);

    m_fromBlob = m_cli->getModelFromBlob(modelName);
    QString realFrom = m_cli->findOtherModelSharingBlob(m_fromBlob, modelName);

    if (params.architecture.isEmpty()) {
        m_statusLabel->setText(T("editor.statusLoadError").arg(modelName));
        m_defaultCtxLabel->clear();
        m_maxCtxLabel->clear();
        return;
    }

    m_fromCombo->blockSignals(true);
    QString baseFrom = modelName;
    if (baseFrom.isEmpty()) {
        if (!params.fromBase.isEmpty())      baseFrom = params.fromBase;
        else if (!realFrom.isEmpty())        baseFrom = realFrom;
        else                                 baseFrom = m_fromBlob;
    }
    if (!baseFrom.isEmpty()) {
        int idx = m_fromCombo->findText(baseFrom);
        if (idx >= 0)
            m_fromCombo->setCurrentIndex(idx);
        else
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
        int wantK = qRound(params.parameters["num_ctx"].toInt() / 1024.0);
        m_numCtxSpin->setValue(wantK);
        if (wantK > maxK)
            m_numCtxSpin->setStyleSheet("QSpinBox { color: red; }");
        else
            m_numCtxSpin->setStyleSheet("");
    }

    m_tempCheck->setChecked(params.parameters.contains("temperature"));
    if (params.parameters.contains("temperature"))
        m_tempSpin->setValue(params.parameters["temperature"].toDouble());

    m_topPCheck->setChecked(params.parameters.contains("top_p"));
    if (params.parameters.contains("top_p"))
        m_topPSpin->setValue(params.parameters["top_p"].toDouble());

    m_topKCheck->setChecked(params.parameters.contains("top_k"));
    if (params.parameters.contains("top_k"))
        m_topKSpin->setValue(params.parameters["top_k"].toInt());

    m_repeatPenaltyCheck->setChecked(params.parameters.contains("repeat_penalty"));
    if (params.parameters.contains("repeat_penalty"))
        m_repeatPenaltySpin->setValue(params.parameters["repeat_penalty"].toDouble());

    m_repeatLastNCheck->setChecked(params.parameters.contains("repeat_last_n"));
    if (params.parameters.contains("repeat_last_n"))
        m_repeatLastNSpin->setValue(params.parameters["repeat_last_n"].toInt());

    m_presencePenaltyCheck->setChecked(params.parameters.contains("presence_penalty"));
    if (params.parameters.contains("presence_penalty"))
        m_presencePenaltySpin->setValue(params.parameters["presence_penalty"].toDouble());

    m_frequencyPenaltyCheck->setChecked(params.parameters.contains("frequency_penalty"));
    if (params.parameters.contains("frequency_penalty"))
        m_frequencyPenaltySpin->setValue(params.parameters["frequency_penalty"].toDouble());

    bool hasSystem = !params.systemPrompt.isEmpty();
    m_systemPromptCheck->setChecked(hasSystem);
    if (hasSystem)
        m_systemPromptEdit->setPlainText(params.systemPrompt);

    setEditMode(modelName);

    m_rendererEdit->setText(params.renderer);
    m_parserEdit->setText(params.parser);
    updateCapabilities(params);
    updateTemplateInfo(params);

    updateContextInfo(m_fromCombo->currentText(), params.contextLength);

    m_statusLabel->setText(T("editor.statusLoaded")
        .arg(modelName, params.architecture, params.paramCount));
}

void ModelEditorWidget::onFromChanged(const QString &baseModel)
{
    if (baseModel.isEmpty()) {
        m_defaultCtxLabel->clear();
        m_maxCtxLabel->clear();
        m_baseMaxContext = 32768;
        m_numCtxSlider->setMaximum(1024);
        m_numCtxSpin->setMaximum(1024);
        return;
    }

    int maxCtx = maxContextForBase(baseModel);
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

    updateContextInfo(baseModel, maxCtx);
}

void ModelEditorWidget::onNumCtxChanged(int value)
{
    if (m_baseMaxContext > 0 && value * 1024 > m_baseMaxContext)
        m_numCtxSpin->setStyleSheet("QSpinBox { color: red; }");
    else
        m_numCtxSpin->setStyleSheet("");
}

void ModelEditorWidget::onPreview()
{
    auto config = collectConfig();
    if (config.from.isEmpty()) {
        QMessageBox::warning(this, T("editor.warnTitle"), T("editor.warnFrom"));
        return;
    }
    QString content = ModelfileBuilder::build(config);
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(T("editor.previewTitle"));
    msgBox.setText(content);
    msgBox.setDetailedText(content);
    msgBox.exec();
}

void ModelEditorWidget::onCreate()
{
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
    QString content = ModelfileBuilder::build(config);
    emit createRequested(name, content);
}

void ModelEditorWidget::onUpdate()
{
    if (m_currentEditModel.isEmpty()) return;

    auto config = collectConfig();
    if (config.from.isEmpty()) {
        QMessageBox::warning(this, T("editor.warnTitle"), T("editor.warnFrom"));
        return;
    }
    QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) return;

    QString content = ModelfileBuilder::build(config);
    emit updateRequested(name, content);
}

ModelfileBuilder::Config ModelEditorWidget::collectConfig()
{
    ModelfileBuilder::Config config;
    config.from = m_fromCombo->currentText().trimmed();

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

int ModelEditorWidget::maxContextForBase(const QString &baseModel)
{
    OllamaCli::ModelParams params = m_cli->showModel(baseModel);
    return params.contextLength;
}

void ModelEditorWidget::updateContextInfo(const QString &baseModel, int loadedContextLength)
{
    if (baseModel.isEmpty()) {
        m_defaultCtxLabel->clear();
        return;
    }

    OllamaCli::ModelParams params = m_cli->showModel(baseModel);
    int defaultCtx = 2048;
    if (params.parameters.contains("num_ctx")) {
        defaultCtx = params.parameters["num_ctx"].toInt();
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

void ModelEditorWidget::toggleParamEnabled(QCheckBox *check, QWidget *widget)
{
    widget->setEnabled(check->isChecked());
}
