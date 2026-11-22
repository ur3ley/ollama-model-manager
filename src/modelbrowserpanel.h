#ifndef MODELBROWSERPANEL_H
#define MODELBROWSERPANEL_H

#include <QHash>
#include <QList>
#include <QStringList>
#include <QWidget>

#include "memoryinfo.h"

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QTextBrowser;
class QTimer;
class QToolButton;

struct BrowserVariant {
    QString id;      // what to install: family:tag or repo/file
    QString label;   // shown in the combo: UD-Q4_K_XL
    QString size;    // 24 GB
    QString hint;    // disabled reason, empty when selectable
    QString context; // 40K — context window from the tag page
    QString input;   // Text / Vision
    bool enabled = true;
    bool installed = false;        // a local copy exists ("скачано")
    bool updateAvailable = false;  // installed, but the source has a newer build
    quint64 sizeBytes = 0;
    memoryinfo::Fit fit = memoryinfo::Fit::Unknown;
};

// "What is this model": the long blurb, the readme and the update age from the
// family page. Source-agnostic on purpose — the window fills it from either
// ollama.com or (later) a HuggingFace model card.
struct BrowserAbout {
    QString description;
    QString readmeMarkdown;
    QString updated;   // "1 year ago"
};

// Per-quantization facts from the tag page.
struct BrowserSpecs {
    QString architecture;  // qwen3moe
    QString parameters;    // 30.5B
    QString quantization;  // Q4_K_M
    QString license;       // Apache License
};

struct BrowserItem {
    QString id;
    QString title;
    QString subtitle;      // author / namespace
    QString description;
    QStringList badges;    // vision, tools, thinking, ...
    QString stats;         // "↓ 494.7K · ♥ 327 · 2h ago"
    QList<BrowserVariant> variants;
    bool variantsLoaded = false;
    BrowserAbout about;
    bool aboutLoaded = false;
    QHash<QString, BrowserSpecs> specs;  // keyed by variant id
};

// "Model hub" style browser: a card list on the left, a detail pane on the
// right (title, badges, characteristics, readme, variant picker, download
// button, description). Source-agnostic: the dialog feeds it either
// ollama.com families or HuggingFace repositories.
class ModelBrowserPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ModelBrowserPanel(const QString &placeholder, QWidget *parent = nullptr);

    void setItems(const QList<BrowserItem> &items);
    void clearItems();
    void setVariants(const QString &itemId, const QList<BrowserVariant> &variants);
    void setVariantsError(const QString &itemId, const QString &error);
    /**
     * Update the install state of one variant in place (no list rebuild, so the
     * user's current pick survives an async update check).
     */
    void setVariantState(const QString &itemId, const QString &variantId,
                         bool installed, bool updateAvailable);
    /** Family page data: readme, long description, "updated … ago". */
    void setModelDetails(const QString &itemId, const BrowserAbout &about);
    /** Tag page data for one variant; ignored for a variant of another item. */
    void setTagDetails(const QString &itemId, const QString &variantId,
                       const BrowserSpecs &specs);
    /** Append a suffix to one variant's label (e.g. "update available"). */
    void patchVariant(const QString &itemId, const QString &variantId, const QString &suffix);
    void selectItem(int row);
    void selectVariant(int index);
    void selectVariantById(const QString &variantId);
    int itemCount() const;
    int variantCount() const;
    QString selectedItemId() const;
    QString selectedVariantId() const;
    /** Declared download size text of the selected variant ("39GB"), empty when unknown. */
    QString selectedVariantSizeText() const;
    /** Label of the selected variant row, including download-state badges. */
    QString selectedVariantText() const;
    void setActionText(const QString &text);
    /** Per-variant download state: the action button says "queued"/"running". */
    enum class JobState { None, Queued, Running };
    void setVariantJobState(const QString &itemId, const QString &variantId, JobState state);
    /** percent < 0 shows an indeterminate bar (total download size unknown). */
    void setBusy(bool busy, int percent, bool cancellable = true, bool queueable = false);
    void setStatus(const QString &status);
    void setEmptyText(const QString &text);
    /** Expand/collapse the "About this model" block (dev aid + tests). */
    void setAboutExpanded(bool expanded);
    bool isAboutExpanded() const;

signals:
    void itemSelected(const QString &itemId);
    void variantSelected(const QString &itemId, const QString &variantId);
    void actionRequested(const QString &itemId, const QString &variantId);
    /** The same button doubles as "Отмена" for a queued/running variant. */
    void cancelVariantRequested(const QString &itemId, const QString &variantId);
    void cancelRequested();

private:
    void updateDetail();
    void updateSpecs();
    void updateAbout();
    void applyAboutStyle();
    void requestCurrentVariant();
    void appendBatch();
    void updateActionState();
    void applyVariantColors();
    /** Combo row text for one variant: label · size · state mark · fit hint. */
    QString variantLabel(const BrowserVariant &variant) const;
    void updateVariantRow(int row, const BrowserVariant &variant);
    const BrowserItem *itemById(const QString &id) const;
    BrowserItem *itemById(const QString &id);

    QListWidget *m_list;
    QLabel *m_emptyLabel;
    QLabel *m_titleLabel;
    QLabel *m_subtitleLabel;
    QWidget *m_badgesRow;
    QLabel *m_statsLabel;
    QLabel *m_specsLabel;
    QLabel *m_descriptionLabel;
    QToolButton *m_aboutToggle;
    QTextBrowser *m_about;
    QComboBox *m_variantCombo;
    QLabel *m_variantSize;
    QPushButton *m_actionBtn;
    QPushButton *m_cancelBtn;
    QProgressBar *m_progress;
    QLabel *m_statusLabel;
    QList<QLabel *> m_chips; // capability badges, restyled on theme change

    QTimer *m_batchTimer = nullptr;
    QList<BrowserItem> m_pendingItems;
    int m_pendingIndex = 0;
    QList<BrowserItem> m_items;
    QString m_selectedId;
    QString m_actionText;
    QString m_requestedVariant; // item:variant already asked for
    // item\nvariant -> download state shown instead of "Скачать". Kept apart
    // from installed flags: a queued/running download is not on disk yet.
    QHash<QString, JobState> m_jobStates;
    QString m_renderedMarkdown; // readme currently in m_about
    bool m_busy = false;
    bool m_busyCancellable = true;
    // A registry pull can be queued: while it runs, the list and the variant
    // picker stay usable so another download can be added to the CLI queue.
    bool m_queueable = false;
};

#endif // MODELBROWSERPANEL_H
