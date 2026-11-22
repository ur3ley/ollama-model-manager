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
#include <QTimer>
#include <QPointer>
#include "ollamacli.h"
#include "modelfilebuilder.h"
#include "i18n.h"
#include "registryclient.h"
#include "repogroup.h"

class InstallService;

class ModelEditorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ModelEditorWidget(InstallService *install, QWidget *parent = nullptr);

    /** Edit an installed repository: FROM = its installed quants. */
    void loadRepo(const RepoGroup &group);
    void loadModel(const QString &modelName);
    void setEditMode(const QString &modelName);
    void setCreateMode();
    void setAvailableModels(const QStringList &models);

    ModelfileBuilder::Config collectConfig();
    QString getModelName() const;
    /** Empty in "new model" mode; the repository id otherwise. */
    QString repoId() const { return m_repo.id; }

signals:
    void createRequested(const QString &name, const QString &modelfileContent);
    void updateRequested(const QString &name, const QString &modelfileContent);
    void renameRequested(const QString &oldName, const QString &newName);
    /** Rename an installed quant and immediately rebuild it with new parameters. */
    void renameAndUpdateRequested(const QString &oldName, const QString &newName,
                                  const QString &modelfileContent);
    /** Local model without provenance: open "bind source…" for this name. */
    void bindSourceRequested(const QString &modelName);
    /** Delete only the quant selected in FROM (the repository stays). */
    void deleteQuantRequested(const QString &modelName);

private slots:
    void onFromChanged(const QString &baseModel);
    void onPreview();
    void onCreate();
    void onUpdate();
    void onNumCtxChanged(int value);
    void onClear();

private:
    void setupUI();
    void retranslateUi();
    void clearForm();
    void updateCapabilities(const OllamaCli::ModelParams &params);
    void updateTemplateInfo(const OllamaCli::ModelParams &params);
    void applyLoadedModel(const QString &modelName, const OllamaCli::ModelParams &params);
    void applyBaseContext(bool ok, const OllamaCli::ModelParams &params);
    void applyContextInfo(const OllamaCli::ModelParams &params, int loadedContextLength);
    void requestBaseContext(const QString &baseModel);
    bool buildModelfile(const ModelfileBuilder::Config &config, QString *content);
    void setDefaultCtxLabel(int defaultCtx);
    void setMaxCtxLabel(int maxCtx);
    void toggleParamEnabled(QCheckBox *check, QWidget *widget);
    void setAdvancedVisible(bool visible);
    void applyFromComboColors();
    void rebuildFromCombo(const QString &select = QString());
    void requestFamilyTags();
    void onFamilyTags(bool ok, const QList<RegistryClient::Tag> &tags, const QString &error);
    void onFromActivated(int index);
    void onFromContextMenu(QPoint pos);
    bool selectedFromInstalled() const;
    void updateModelButtons();
    void updateDeleteQuantState();
    void installSelectedQuant();
    void updateModelRowVisibility();
    QString currentFromName() const;
    void updateNameForSelection();
    void updateSourceLabel();
    void applyRepoModeToFrom();

    InstallService *m_install;
    OllamaCli *m_cli;

    QLabel *m_titleLabel;
    QLabel *m_sourceLabel;
    QLabel *m_unknownSourceLabel;
    QPushButton *m_bindSourceBtn;
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
    QPointer<OllamaJob> m_installJob;  // our quant pull (A-1: no name matching)
    QComboBox *m_fromCombo;
    QWidget *m_fromRowWidget = nullptr;
    QToolButton *m_deleteQuantBtn = nullptr;
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
    RepoGroup m_repo;
    QStringList m_capabilities;
    bool m_templateSoftSwitch = false;
    // `ollama show` runs asynchronously: the names whose replies are awaited.
    // A reply for any other name is stale (the user moved on) and is dropped.
    QString m_loadingModel;
    QString m_ctxBase;
    QPointer<OllamaJob> m_loadJob;  // `ollama show` of the selected model
    QPointer<OllamaJob> m_ctxJob;   // `ollama show` of the FROM base
    QTimer *m_ctxTimer = nullptr;
    int m_baseMaxContext = 32768;
};

#endif // MODELEDITORWIDGET_H
