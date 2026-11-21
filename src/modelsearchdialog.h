#ifndef MODELSEARCHDIALOG_H
#define MODELSEARCHDIALOG_H

#include <QDialog>
#include <QList>

#include <QHash>
#include <QSet>

#include "hfclient.h"
#include "modelfilters.h"
#include "registryclient.h"

class OllamaCli;
class ModelBrowserPanel;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTabWidget;

// "Model hub"-style window: search a source, browse cards on the left, see the
// details and install a variant on the right.
//
//   * Ollama tab  — ollama.com library, tags as variants, install via pull;
//   * HuggingFace — GGUF repositories, quants as variants, download then
//                   hand the file to `ollama create` through the signal.
class ModelSearchDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ModelSearchDialog(OllamaCli *cli, QWidget *parent = nullptr);

    // Hooks for the offscreen smoke test.
    void searchAs(const QString &query);
    void selectModel(int row);
    void selectTag(int row);
    int modelCount() const;
    int tagCount() const;
    QString suggestedName() const;
    QString statusText() const;
    void startInstall();
    void showHfTab();
    int hfModelCount() const;
    int hfVariantCount() const;
    void selectHfModel(int row);
    void showDeviceTab();
    int deviceCount() const;
    int deviceVariantCount() const;
    void selectDevice(int row);

signals:
    void modelCreateRequested(const QString &name, const QString &modelfileContent);
    void modelsChanged();

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void onOllamaSearch();
    void onOllamaSearchFinished(bool ok, const QList<RegistryClient::Model> &models,
                                const QString &error);
    void onOllamaItemSelected(const QString &itemId);
    void onTagsFinished(bool ok, const QList<RegistryClient::Tag> &tags, const QString &error);
    void onOllamaAction(const QString &itemId, const QString &variantId);
    void onPullProgress(const QString &status, int percent);
    void onPullFinished(bool ok, const QString &output);

    void onHfSearch();
    void onHfSearchFinished(bool ok, const QList<HfClient::Repo> &repos, const QString &error);
    void onHfItemSelected(const QString &itemId);
    void onHfFilesFinished(bool ok, const QList<HfClient::File> &files, const QString &error);
    void onHfAction(const QString &repo, const QString &file);
    void onHfProgress(qint64 received, qint64 total);
    void onHfFinished(bool ok, const QString &path, const QString &error);

    void reloadDevice();
    void refreshInstalled();
    void onDeviceItemSelected(const QString &itemId);
    void onDeviceAction(const QString &itemId, const QString &variantId);
    void onManifestChecked(const QString &ref, const QString &digest, bool ok, const QString &error);
    void onRepoInfoFinished(const QString &repo, const QString &sha, bool ok, const QString &error);
    void setDeviceVariant(const QString &itemId, bool upToDate, bool ok, const QString &error);
    void applyDeviceUpdateMark();

private:
    void setupUi();
    void ensureLoaded();
    void applyOllamaSort(QList<RegistryClient::Model> &models) const;
    void rebuildOllamaItems();
    void rebuildHfItems();
    void openFilters();
    void updateFilterButton();
    QString hfSortKey() const;

    OllamaCli *m_cli;
    RegistryClient *m_registry;
    HfClient *m_hf;

    QTabWidget *m_tabs;
    QLineEdit *m_queryEdit;
    QPushButton *m_searchBtn;
    QComboBox *m_sortCombo;
    ModelBrowserPanel *m_ollamaPanel;

    QLineEdit *m_hfQueryEdit;
    QPushButton *m_hfSearchBtn;
    QComboBox *m_hfSortCombo;
    QPushButton *m_filterBtn;
    QPushButton *m_filterBtnHf;
    ModelBrowserPanel *m_hfPanel;

    QWidget *m_deviceTab;
    ModelBrowserPanel *m_devicePanel;
    QPushButton *m_deviceRefreshBtn;
    QPushButton *m_closeBtn;

    QString m_pendingTagsId;      // panel item that will receive variants
    QString m_pendingTagsFamily;  // family/ref to query
    bool m_tagsForDevice = false;
    QString m_pendingRepo;
    QString m_pendingFile;
    QString m_pendingFilesItemId; // panel item that will receive variants
    bool m_filesForDevice = false;
    QString m_hfQuery;
    QString m_downloadDir;
    QString m_pendingModelName;
    QString m_pullingName;
    ModelBrowserPanel *m_activePanel = nullptr;
    QList<RegistryClient::Model> m_ollamaResults;
    QList<HfClient::Repo> m_hfResults;
    ModelFilters m_filters;
    QHash<QString, QString> m_repoShas;
    QHash<QString, QString> m_localIds;
    QSet<QString> m_installedNames;
    QString m_deviceCurrentId;
    QString m_deviceInstalledVariant;
    bool m_deviceUpToDateKnown = false;
    bool m_deviceUpToDate = false;
    bool m_ollamaLoaded = false;
    bool m_hfLoaded = false;
    bool m_deviceLoaded = false;
};

#endif // MODELSEARCHDIALOG_H
