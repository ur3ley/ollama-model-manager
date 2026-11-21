#ifndef MODELEDITORWIDGET_H
#define MODELEDITORWIDGET_H

#include <QWidget>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QCheckBox>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include "ollamacli.h"
#include "modelfilebuilder.h"
#include "i18n.h"
#include "registryclient.h"

class ModelEditorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ModelEditorWidget(OllamaCli *cli, QWidget *parent = nullptr);

    void loadModel(const QString &modelName);
    void setEditMode(const QString &modelName);
    void setCreateMode();
    void setAvailableModels(const QStringList &models);

    ModelfileBuilder::Config collectConfig();
    QString getModelName() const;

signals:
    void createRequested(const QString &name, const QString &modelfileContent);
    void updateRequested(const QString &name, const QString &modelfileContent);
    void renameRequested(const QString &oldName, const QString &newName);

private slots:
    void onFromChanged(const QString &baseModel);
    void onPreview();
    void onCreate();
    void onUpdate();
    void onNumCtxChanged(int value);

private:
    void setupUI();
    void retranslateUi();
    void clearForm();
    void updateCapabilities(const OllamaCli::ModelParams &params);
    void updateTemplateInfo(const OllamaCli::ModelParams &params);
    int maxContextForBase(const QString &baseModel);
    void updateContextInfo(const QString &baseModel, int loadedContextLength);
    void setDefaultCtxLabel(int defaultCtx);
    void setMaxCtxLabel(int maxCtx);
    void toggleParamEnabled(QCheckBox *check, QWidget *widget);
    void setAdvancedVisible(bool visible);
    void rebuildFromCombo(const QString &select = QString());
    void requestFamilyTags();
    void onFamilyTags(bool ok, const QList<RegistryClient::Tag> &tags, const QString &error);
    void onFromActivated(int index);
    void onFromContextMenu(const QPoint &pos);
    bool selectedFromInstalled() const;
    void updateModelButtons();
    void installSelectedQuant();
    void updateModelRowVisibility();

    OllamaCli *m_cli;

    QLabel *m_titleLabel;
    QLabel *m_capsLabel;
    QGroupBox *m_modelGroup;
    QFormLayout *m_modelLayout;
    QLabel *m_templateWarnLabel;
    QGroupBox *m_ctxGroup;
    QLabel *m_ctxUnitLabel;
    QGroupBox *m_sampleGroup;
    QGroupBox *m_penaltyGroup;
    QGroupBox *m_sysGroup;

    RegistryClient *m_registry;
    QList<RegistryClient::Tag> m_familyTags;
    QString m_family;
    QString m_modelStem;
    QStringList m_localModels;
    QString m_pendingInstall;
    QComboBox *m_fromCombo;
    QLineEdit *m_nameEdit;
    QLineEdit *m_rendererEdit;
    QLineEdit *m_parserEdit;
    QLabel *m_modelInfoLabel;
    QToolButton *m_advancedBtn;
    QLabel *m_defaultCtxLabel;
    QLabel *m_maxCtxLabel;

    QCheckBox *m_numCtxCheck;
    QSlider *m_numCtxSlider;
    QSpinBox *m_numCtxSpin;

    QCheckBox *m_tempCheck;
    QDoubleSpinBox *m_tempSpin;
    QCheckBox *m_topPCheck;
    QDoubleSpinBox *m_topPSpin;
    QCheckBox *m_topKCheck;
    QSpinBox *m_topKSpin;
    QCheckBox *m_repeatPenaltyCheck;
    QDoubleSpinBox *m_repeatPenaltySpin;
    QCheckBox *m_repeatLastNCheck;
    QSpinBox *m_repeatLastNSpin;
    QCheckBox *m_presencePenaltyCheck;
    QDoubleSpinBox *m_presencePenaltySpin;
    QCheckBox *m_frequencyPenaltyCheck;
    QDoubleSpinBox *m_frequencyPenaltySpin;

    QCheckBox *m_systemPromptCheck;
    QPlainTextEdit *m_systemPromptEdit;

    QPushButton *m_previewBtn;
    QPushButton *m_createBtn;
    QPushButton *m_updateBtn;
    QPushButton *m_clearBtn;

    QLabel *m_statusLabel;
    QString m_currentEditModel;
    QStringList m_capabilities;
    bool m_templateSoftSwitch = false;
    QString m_fromBlob;  // direct blob path for updates, bypasses inheritance
    int m_baseMaxContext = 32768;
};

#endif // MODELEDITORWIDGET_H
