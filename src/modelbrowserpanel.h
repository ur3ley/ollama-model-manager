#ifndef MODELBROWSERPANEL_H
#define MODELBROWSERPANEL_H

#include <QList>
#include <QStringList>
#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QTimer;

struct BrowserVariant {
    QString id;      // what to install: family:tag or repo/file
    QString label;   // shown in the combo: UD-Q4_K_XL
    QString size;    // 24 GB
    QString hint;    // disabled reason, empty when selectable
    bool enabled = true;
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
};

// "Model hub" style browser: a card list on the left, a detail pane on the
// right (title, badges, variant picker, download button, description).
// Source-agnostic: the dialog feeds it either ollama.com families or
// HuggingFace repositories.
class ModelBrowserPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ModelBrowserPanel(const QString &placeholder, QWidget *parent = nullptr);

    void setItems(const QList<BrowserItem> &items);
    void clearItems();
    void setVariants(const QString &itemId, const QList<BrowserVariant> &variants);
    void setVariantsError(const QString &itemId, const QString &error);
    /** Append a suffix to one variant's label (e.g. "update available"). */
    void patchVariant(const QString &itemId, const QString &variantId, const QString &suffix);
    void selectItem(int row);
    void selectVariant(int index);
    void selectVariantById(const QString &variantId);
    int itemCount() const;
    int variantCount() const;
    QString selectedItemId() const;
    QString selectedVariantId() const;
    void setActionText(const QString &text);
    void setBusy(bool busy, int percent);
    void setStatus(const QString &status);
    void setEmptyText(const QString &text);

signals:
    void itemSelected(const QString &itemId);
    void actionRequested(const QString &itemId, const QString &variantId);
    void cancelRequested();

private:
    void updateDetail();
    void appendBatch();
    void updateActionState();
    const BrowserItem *itemById(const QString &id) const;
    BrowserItem *itemById(const QString &id);

    QListWidget *m_list;
    QLabel *m_emptyLabel;
    QLabel *m_titleLabel;
    QLabel *m_subtitleLabel;
    QWidget *m_badgesRow;
    QLabel *m_statsLabel;
    QLabel *m_descriptionLabel;
    QComboBox *m_variantCombo;
    QLabel *m_variantSize;
    QPushButton *m_actionBtn;
    QPushButton *m_cancelBtn;
    QProgressBar *m_progress;
    QLabel *m_statusLabel;

    QTimer *m_batchTimer = nullptr;
    QList<BrowserItem> m_pendingItems;
    int m_pendingIndex = 0;
    QList<BrowserItem> m_items;
    QString m_selectedId;
    QString m_actionText;
    bool m_busy = false;
};

#endif // MODELBROWSERPANEL_H
