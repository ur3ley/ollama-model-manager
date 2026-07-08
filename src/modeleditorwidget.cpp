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

    auto *title = new QLabel(tr("Редактор модели"));
    QFont boldFont = title->font();
    boldFont.setBold(true);
    boldFont.setPointSize(boldFont.pointSize() + 2);
    title->setFont(boldFont);
    mainLayout->addWidget(title);

    auto *scrollArea = new QScrollArea();
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *scrollWidget = new QWidget();
    auto *form = new QVBoxLayout(scrollWidget);

    auto *modelGroup = new QGroupBox(tr("Модель"));
    auto *modelLayout = new QFormLayout(modelGroup);

    m_fromCombo = new QComboBox();
    m_fromCombo->setEditable(true);
    m_fromCombo->setToolTip(tr(
        "Базовая модель, от которой наследуются все параметры.\n"
        "Выберите из списка установленных или введите имя вручную\n"
        "(например, для модели из реестра Ollama)."
    ));
    modelLayout->addRow(tr("FROM:"), m_fromCombo);

    m_nameEdit = new QLineEdit();
    m_nameEdit->setPlaceholderText(tr("напр. my-model:variant"));
    m_nameEdit->setToolTip(tr("Имя новой модели"));
    m_nameEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression("[a-zA-Z0-9._/:\\-]*"), m_nameEdit));
    modelLayout->addRow(tr("Имя:"), m_nameEdit);

    form->addWidget(modelGroup);

    auto *ctxGroup = new QGroupBox(tr("Контекст"));
    auto *ctxLayout = new QVBoxLayout(ctxGroup);

    auto *ctxRow = new QHBoxLayout();
    m_numCtxCheck = new QCheckBox(tr("num_ctx:"));
    m_numCtxCheck->setChecked(true);
    m_numCtxCheck->setToolTip(tr(
        "Размер окна контекста в тысячах токенов (K = 1024).\n"
        "Определяет, сколько предыдущих токенов\n"
        "модель «видит» при генерации ответа.\n"
        "Больше = длиннее диалог, но больше памяти.\n"
        "Типичные значения: 4K, 8K, 32K, 128K."
    ));
    ctxRow->addWidget(m_numCtxCheck);

    m_numCtxSlider = new QSlider(Qt::Horizontal);
    m_numCtxSlider->setRange(2, 1024);
    m_numCtxSlider->setSingleStep(1);
    m_numCtxSlider->setPageStep(4);
    m_numCtxSlider->setToolTip(tr("Быстрая настройка размера контекста (K)"));
    ctxRow->addWidget(m_numCtxSlider, 7);

    m_numCtxSpin = new QSpinBox();
    m_numCtxSpin->setRange(2, 1024);
    m_numCtxSpin->setSingleStep(1);
    m_numCtxSpin->setToolTip(tr("Размер окна контекста в K (1K = 1024 токена)"));
    ctxRow->addWidget(m_numCtxSpin, 3);

    auto *ctxUnitLabel = new QLabel(tr("K"));
    ctxRow->addWidget(ctxUnitLabel);
    ctxLayout->addLayout(ctxRow);

    m_defaultCtxLabel = new QLabel();
    m_defaultCtxLabel->setStyleSheet("color: #666; font-size: 11px; padding-left: 4px;");
    ctxLayout->addWidget(m_defaultCtxLabel);

    m_maxCtxLabel = new QLabel();
    m_maxCtxLabel->setStyleSheet("color: #666; font-size: 11px; padding-left: 4px;");
    ctxLayout->addWidget(m_maxCtxLabel);

    form->addWidget(ctxGroup);

    auto *sampleGroup = new QGroupBox(tr("Семплирование"));
    auto *sampleGrid = new QGridLayout(sampleGroup);

    m_tempCheck = new QCheckBox(tr("temperature:"));
    sampleGrid->addWidget(m_tempCheck, 0, 0);
    m_tempSpin = new QDoubleSpinBox();
    m_tempSpin->setRange(0.0, 5.0);
    m_tempSpin->setSingleStep(0.05);
    m_tempSpin->setDecimals(2);
    m_tempSpin->setEnabled(false);
    m_tempSpin->setToolTip(tr(
        "Температура (temperature) — управляет случайностью ответов.\n"
        "0.0 = строго детерминированные ответы (хорошо для кода/фактов).\n"
        "0.5–0.7 = сбалансированная креативность.\n"
        "1.0+ = высокая случайность, творческие ответы.\n"
        "Выше 1.5 = может генерировать бессвязный текст."
    ));
    sampleGrid->addWidget(m_tempSpin, 0, 1);

    m_topPCheck = new QCheckBox(tr("top_p:"));
    sampleGrid->addWidget(m_topPCheck, 0, 2);
    m_topPSpin = new QDoubleSpinBox();
    m_topPSpin->setRange(0.0, 1.0);
    m_topPSpin->setSingleStep(0.05);
    m_topPSpin->setDecimals(2);
    m_topPSpin->setEnabled(false);
    m_topPSpin->setToolTip(tr(
        "top_p (nucleus sampling) — порог накопленной вероятности.\n"
        "Модель выбирает из токенов, чья суммарная вероятность ≤ top_p.\n"
        "0.9 = рассматриваются токены, дающие 90% вероятности.\n"
        "Ниже = более предсказуемо. 1.0 = без ограничения."
    ));
    sampleGrid->addWidget(m_topPSpin, 0, 3);

    m_topKCheck = new QCheckBox(tr("top_k:"));
    sampleGrid->addWidget(m_topKCheck, 1, 0);
    m_topKSpin = new QSpinBox();
    m_topKSpin->setRange(1, 200);
    m_topKSpin->setEnabled(false);
    m_topKSpin->setToolTip(tr(
        "top_k — ограничивает выбор K наиболее вероятными токенами.\n"
        "40 = модель выбирает только из 40 самых вероятных токенов.\n"
        "Ниже = более консервативные ответы.\n"
        "Выше = больше разнообразия."
    ));
    sampleGrid->addWidget(m_topKSpin, 1, 1);

    m_repeatPenaltyCheck = new QCheckBox(tr("repeat_penalty:"));
    sampleGrid->addWidget(m_repeatPenaltyCheck, 1, 2);
    m_repeatPenaltySpin = new QDoubleSpinBox();
    m_repeatPenaltySpin->setRange(0.0, 2.0);
    m_repeatPenaltySpin->setSingleStep(0.05);
    m_repeatPenaltySpin->setDecimals(2);
    m_repeatPenaltySpin->setEnabled(false);
    m_repeatPenaltySpin->setToolTip(tr(
        "repeat_penalty — штраф за повторение токенов.\n"
        "1.0 = без штрафа. > 1.0 = снижает вероятность\n"
        "уже использованных токенов. Помогает избежать\n"
        "зацикливания и повторений в ответах."
    ));
    sampleGrid->addWidget(m_repeatPenaltySpin, 1, 3);

    m_repeatLastNCheck = new QCheckBox(tr("repeat_last_n:"));
    sampleGrid->addWidget(m_repeatLastNCheck, 2, 0);
    m_repeatLastNSpin = new QSpinBox();
    m_repeatLastNSpin->setRange(0, 2048);
    m_repeatLastNSpin->setEnabled(false);
    m_repeatLastNSpin->setToolTip(tr(
        "repeat_last_n — на сколько токенов назад смотреть\n"
        "при проверке повторений (для repeat_penalty).\n"
        "64 = проверять последние 64 токена.\n"
        "0 = смотреть на всю историю."
    ));
    sampleGrid->addWidget(m_repeatLastNSpin, 2, 1);

    form->addWidget(sampleGroup);

    auto *penaltyGroup = new QGroupBox(tr("Штрафы"));
    auto *penaltyGrid = new QGridLayout(penaltyGroup);

    m_presencePenaltyCheck = new QCheckBox(tr("presence_penalty:"));
    penaltyGrid->addWidget(m_presencePenaltyCheck, 0, 0);
    m_presencePenaltySpin = new QDoubleSpinBox();
    m_presencePenaltySpin->setRange(-2.0, 2.0);
    m_presencePenaltySpin->setSingleStep(0.05);
    m_presencePenaltySpin->setDecimals(2);
    m_presencePenaltySpin->setEnabled(false);
    m_presencePenaltySpin->setToolTip(tr(
        "presence_penalty — штраф за уже встречавшиеся токены.\n"
        "Положительное значение = модель реже повторяет\n"
        "уже сказанное, побуждая к новым темам.\n"
        "Отрицательное = модель чаще возвращается\n"
        "к уже упомянутым темам (больше связности)."
    ));
    penaltyGrid->addWidget(m_presencePenaltySpin, 0, 1);

    m_frequencyPenaltyCheck = new QCheckBox(tr("frequency_penalty:"));
    penaltyGrid->addWidget(m_frequencyPenaltyCheck, 0, 2);
    m_frequencyPenaltySpin = new QDoubleSpinBox();
    m_frequencyPenaltySpin->setRange(-2.0, 2.0);
    m_frequencyPenaltySpin->setSingleStep(0.05);
    m_frequencyPenaltySpin->setDecimals(2);
    m_frequencyPenaltySpin->setEnabled(false);
    m_frequencyPenaltySpin->setToolTip(tr(
        "frequency_penalty — штраф, пропорциональный\n"
        "частоте токена в тексте. Сильнее давит на частые\n"
        "слова, чем presence_penalty.\n"
        "Положительное = меньше повторений слов.\n"
        "Отрицательное = больше повторений."
    ));
    penaltyGrid->addWidget(m_frequencyPenaltySpin, 0, 3);

    form->addWidget(penaltyGroup);

    auto *sysGroup = new QGroupBox(tr("Системный промпт"));
    auto *sysLayout = new QVBoxLayout(sysGroup);

    auto *sysHeader = new QHBoxLayout();
    m_systemPromptCheck = new QCheckBox(tr("Включить системный промпт"));
    sysHeader->addWidget(m_systemPromptCheck);
    sysHeader->addStretch();
    sysLayout->addLayout(sysHeader);

    m_systemPromptEdit = new QPlainTextEdit();
    m_systemPromptEdit->setPlaceholderText(tr("Необязательный системный промпт..."));
    m_systemPromptEdit->setMaximumHeight(180);
    m_systemPromptEdit->setEnabled(false);
    sysLayout->addWidget(m_systemPromptEdit);

    form->addWidget(sysGroup);

    form->addStretch();

    scrollArea->setWidget(scrollWidget);
    mainLayout->addWidget(scrollArea, 1);

    auto *btnLayout = new QHBoxLayout();
    m_previewBtn = new QPushButton(tr("Предпросмотр"));
    m_previewBtn->setToolTip(tr("Показать сгенерированный Modelfile без создания модели"));
    btnLayout->addWidget(m_previewBtn);

    m_createBtn = new QPushButton(tr("Создать модель"));
    m_createBtn->setStyleSheet("QPushButton { font-weight: bold; }");
    m_createBtn->setToolTip(tr("Создать новую модель с этими параметрами"));
    btnLayout->addWidget(m_createBtn);

    m_updateBtn = new QPushButton(tr("Обновить модель"));
    m_updateBtn->setToolTip(tr("Пересоздать текущую модель с новыми параметрами"));
    m_updateBtn->setVisible(false);
    btnLayout->addWidget(m_updateBtn);

    m_clearBtn = new QPushButton(tr("Очистить"));
    m_clearBtn->setToolTip(tr("Сбросить все поля"));
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
}

void ModelEditorWidget::setAvailableModels(const QStringList &models)
{
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
    m_statusLabel->setText(tr("Создание новой модели"));

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
    m_statusLabel->setText(tr("Редактирование: %1").arg(modelName));
}

void ModelEditorWidget::loadModel(const QString &modelName)
{
    OllamaCli::ModelParams params = m_cli->showModel(modelName);

    m_fromBlob = m_cli->getModelFromBlob(modelName);
    QString realFrom = m_cli->findOtherModelSharingBlob(m_fromBlob, modelName);

    if (params.architecture.isEmpty()) {
        m_statusLabel->setText(tr("Ошибка загрузки: %1").arg(modelName));
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

    updateContextInfo(m_fromCombo->currentText(), params.contextLength);

    m_statusLabel->setText(tr("Загружено: %1 (%2, %3 параметров)")
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
        QMessageBox::warning(this, tr("Предупреждение"), tr("Выберите базовую модель (FROM)."));
        return;
    }
    QString content = ModelfileBuilder::build(config);
    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Предпросмотр Modelfile"));
    msgBox.setText(content);
    msgBox.setDetailedText(content);
    msgBox.exec();
}

void ModelEditorWidget::onCreate()
{
    auto config = collectConfig();
    if (config.from.isEmpty()) {
        QMessageBox::warning(this, tr("Предупреждение"), tr("Выберите базовую модель (FROM)."));
        return;
    }
    QString name = m_nameEdit->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Предупреждение"), tr("Введите имя модели."));
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
        QMessageBox::warning(this, tr("Предупреждение"), tr("Выберите базовую модель (FROM)."));
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
        m_defaultCtxLabel->setText(tr("По умолчанию у базовой модели: %1 токенов (%2K)")
            .arg(defaultCtx).arg(defaultCtx / 1024));
    } else {
        m_defaultCtxLabel->clear();
    }
}

void ModelEditorWidget::setMaxCtxLabel(int maxCtx)
{
    if (maxCtx > 0) {
        m_maxCtxLabel->setText(tr("Аппаратный максимум: %1 токенов (%2K)")
            .arg(maxCtx).arg(maxCtx / 1024));
    } else {
        m_maxCtxLabel->clear();
    }
}

void ModelEditorWidget::toggleParamEnabled(QCheckBox *check, QWidget *widget)
{
    widget->setEnabled(check->isChecked());
}
