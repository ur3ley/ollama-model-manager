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
#include <QVBoxLayout>
#include <QFormLayout>
#include "ollamacli.h"
#include "modelfilebuilder.h"
#include "i18n.h"

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
    int maxContextForBase(const QString &baseModel);
    void updateContextInfo(const QString &baseModel, int loadedContextLength);
    void setDefaultCtxLabel(int defaultCtx);
    void setMaxCtxLabel(int maxCtx);
    void toggleParamEnabled(QCheckBox *check, QWidget *widget);

    OllamaCli *m_cli;

    QLabel *m_titleLabel;
    QGroupBox *m_modelGroup;
    QFormLayout *m_modelLayout;
    QGroupBox *m_ctxGroup;
    QLabel *m_ctxUnitLabel;
    QGroupBox *m_sampleGroup;
    QGroupBox *m_penaltyGroup;
    QGroupBox *m_sysGroup;

    QComboBox *m_fromCombo;
    QLineEdit *m_nameEdit;
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
    QString m_fromBlob;  // direct blob path for updates, bypasses inheritance
    int m_baseMaxContext = 32768;
};

#endif // MODELEDITORWIDGET_H
