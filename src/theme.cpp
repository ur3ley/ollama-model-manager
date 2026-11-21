#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace theme {
namespace {

QString configPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return QDir(base).filePath(QStringLiteral("ollama-manager/config.ini"));
}

QPalette buildPalette(const Palette &p)
{
    QPalette pal;
    pal.setColor(QPalette::Window, p.bg);
    pal.setColor(QPalette::WindowText, p.text);
    pal.setColor(QPalette::Base, p.surface);
    pal.setColor(QPalette::AlternateBase, p.surfaceAlt);
    pal.setColor(QPalette::Text, p.text);
    pal.setColor(QPalette::Button, p.surface);
    pal.setColor(QPalette::ButtonText, p.text);
    pal.setColor(QPalette::BrightText, p.danger);
    pal.setColor(QPalette::Highlight, p.accentFill);
    pal.setColor(QPalette::HighlightedText, p.onAccent);
    pal.setColor(QPalette::ToolTipBase, p.surface);
    pal.setColor(QPalette::ToolTipText, p.text);
    pal.setColor(QPalette::PlaceholderText, p.faint);
    pal.setColor(QPalette::Link, p.accent);
    pal.setColor(QPalette::LinkVisited, p.accent);
    pal.setColor(QPalette::Light, p.surfaceAlt);
    pal.setColor(QPalette::Midlight, p.surfaceAlt);
    pal.setColor(QPalette::Mid, p.border);
    pal.setColor(QPalette::Dark, p.borderStrong);
    pal.setColor(QPalette::Shadow, p.borderStrong);

    pal.setColor(QPalette::Disabled, QPalette::Text, p.faint);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, p.faint);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, p.faint);
    pal.setColor(QPalette::Disabled, QPalette::Base, p.surfaceAlt);
    pal.setColor(QPalette::Disabled, QPalette::Button, p.surfaceAlt);
    pal.setColor(QPalette::Disabled, QPalette::Highlight, p.border);
    pal.setColor(QPalette::Disabled, QPalette::HighlightedText, p.faint);
    return pal;
}

QString sub(QString text, const char *token, const QColor &color)
{
    text.replace(QLatin1String(token), color.name());
    return text;
}

} // namespace

Palette lightPalette()
{
    Palette p;
    p.bg = QColor(QStringLiteral("#f3f4f6"));
    p.surface = QColor(QStringLiteral("#ffffff"));
    p.surfaceAlt = QColor(QStringLiteral("#f8f9fb"));
    p.popup = QColor(QStringLiteral("#f8f9fb"));
    p.card = QColor(QStringLiteral("#ffffff"));
    p.cardHover = QColor(QStringLiteral("#f5f7fa"));
    p.selected = QColor(QStringLiteral("#e9f0ff"));
    p.text = QColor(QStringLiteral("#171a1f"));
    p.muted = QColor(QStringLiteral("#6b7280"));
    p.faint = QColor(QStringLiteral("#9ca3af"));
    p.border = QColor(QStringLiteral("#e2e5ea"));
    p.borderStrong = QColor(QStringLiteral("#cdd3db"));
    p.accent = QColor(QStringLiteral("#2563eb"));
    p.accentHover = QColor(QStringLiteral("#1d4ed8"));
    p.accentFill = QColor(QStringLiteral("#2563eb"));
    p.accentSoft = QColor(QStringLiteral("#e6efff"));
    p.onAccent = QColor(QStringLiteral("#ffffff"));
    p.success = QColor(QStringLiteral("#15803d"));
    p.warning = QColor(QStringLiteral("#b45309"));
    p.danger = QColor(QStringLiteral("#b91c1c"));
    p.isDark = false;
    return p;
}

Palette darkPalette()
{
    Palette p;
    p.bg = QColor(QStringLiteral("#121417"));
    p.surface = QColor(QStringLiteral("#1a1d22"));
    p.surfaceAlt = QColor(QStringLiteral("#20242a"));
    p.popup = QColor(QStringLiteral("#262b33"));
    p.card = QColor(QStringLiteral("#1d2127"));
    p.cardHover = QColor(QStringLiteral("#252a32"));
    p.selected = QColor(QStringLiteral("#1e2b41"));
    p.text = QColor(QStringLiteral("#e7eaf0"));
    p.muted = QColor(QStringLiteral("#9aa3b1"));
    p.faint = QColor(QStringLiteral("#6f7885"));
    p.border = QColor(QStringLiteral("#2b313a"));
    p.borderStrong = QColor(QStringLiteral("#3d4450"));
    p.accent = QColor(QStringLiteral("#6aa1ff"));
    p.accentHover = QColor(QStringLiteral("#4c8dff"));
    p.accentFill = QColor(QStringLiteral("#2b64d9"));
    p.accentSoft = QColor(QStringLiteral("#1c2b42"));
    p.onAccent = QColor(QStringLiteral("#ffffff"));
    p.success = QColor(QStringLiteral("#34d399"));
    p.warning = QColor(QStringLiteral("#fbbf24"));
    p.danger = QColor(QStringLiteral("#f87171"));
    p.isDark = true;
    return p;
}

Theme::Theme(QObject *parent)
    : QObject(parent)
{
    const QString env = qEnvironmentVariable("OMM_THEME").trimmed().toLower();
    if (env == QLatin1String("light") || env == QLatin1String("dark")) {
        m_mode = env == QLatin1String("dark") ? Mode::Dark : Mode::Light;
        m_envOverride = true;
    } else {
        QSettings ini(configPath(), QSettings::IniFormat);
        m_mode = modeFromString(ini.value(QStringLiteral("ui/theme")).toString(), Mode::System);
    }
    resolvePalette();

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
                [this](Qt::ColorScheme) { onColorSchemeChanged(); });
    }
#endif
}

Theme *Theme::instance()
{
    static Theme *self = new Theme();
    return self;
}

QString Theme::modeToString(Mode mode)
{
    switch (mode) {
    case Mode::Light: return QStringLiteral("light");
    case Mode::Dark: return QStringLiteral("dark");
    case Mode::System: break;
    }
    return QStringLiteral("system");
}

Mode Theme::modeFromString(const QString &text, Mode fallback)
{
    const QString value = text.trimmed().toLower();
    if (value == QLatin1String("light"))
        return Mode::Light;
    if (value == QLatin1String("dark"))
        return Mode::Dark;
    if (value == QLatin1String("system"))
        return Mode::System;
    return fallback;
}

Mode Theme::effectiveMode() const
{
    if (m_mode != Mode::System)
        return m_mode;
    return m_palette.isDark ? Mode::Dark : Mode::Light;
}

void Theme::resolvePalette()
{
    bool dark = false;
    if (m_mode == Mode::Dark) {
        dark = true;
    } else if (m_mode == Mode::Light) {
        dark = false;
    } else if (auto *hints = QGuiApplication::styleHints()) {
        dark = hints->colorScheme() == Qt::ColorScheme::Dark;
    }
    m_palette = dark ? darkPalette() : lightPalette();
}

void Theme::install()
{
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app)
        return;
    resolvePalette();
    app->setPalette(buildPalette(m_palette));
    app->setStyleSheet(styleSheet());
}

void Theme::setMode(Mode mode)
{
    if (mode != m_mode) {
        m_mode = mode;
        resolvePalette();
    }
    install();
    if (!m_envOverride) {
        QSettings ini(configPath(), QSettings::IniFormat);
        ini.setValue(QStringLiteral("ui/theme"), modeToString(mode));
        ini.sync();
    }
    emit changed();
}

void Theme::onColorSchemeChanged()
{
    if (m_mode != Mode::System)
        return;
    resolvePalette();
    install();
    emit changed();
}

QString styleSheet()
{
    const Palette &p = Theme::instance()->palette();

    QString css = QStringLiteral(R"QSS(
QMainWindow, QDialog {
    background: @bg;
}
QLabel {
    color: @text;
    background: transparent;
}
QLabel[muted="true"] { color: @muted; }
QLabel[caption="true"] { font-size: 11px; }
QLabel[indent="true"] { padding-left: 4px; }
QLabel[section="true"] { color: @muted; font-size: 11px; font-weight: 600; }
QLabel[role="danger"] { color: @danger; }
QLabel[role="warning"] { color: @warning; }
QLabel[role="success"] { color: @success; }

QMenuBar {
    background: @bg;
    color: @text;
    border-bottom: 1px solid @border;
    padding: 2px 6px;
}
QMenuBar::item {
    background: transparent;
    padding: 5px 10px;
    border-radius: 6px;
}
QMenuBar::item:selected { background: @accentSoft; color: @text; }
QMenu {
    background: @popup;
    color: @text;
    border: 1px solid @borderStrong;
    border-radius: 10px;
    padding: 6px;
}
QMenu::item {
    padding: 6px 26px 6px 14px;
    border-radius: 6px;
}
QMenu::item:selected { background: @accentSoft; color: @text; }
QMenu::item:disabled { color: @faint; }
QMenu::separator { height: 1px; background: @border; margin: 6px 8px; }

QToolTip {
    max-width: 420px;
    padding: 6px 10px;
    border: 1px solid @borderStrong;
    border-radius: 6px;
    background: @popup;
    color: @text;
    font-size: 12px;
}

QTabWidget::pane {
    background: @surface;
    border: 1px solid @border;
    border-radius: 12px;
}
QTabWidget::tab-bar { left: 8px; }
QTabBar { background: transparent; }
QTabBar::tab {
    background: transparent;
    color: @muted;
    border: none;
    padding: 7px 18px;
    margin: 4px 3px 4px 0;
    border-radius: 8px;
    font-size: 13px;
}
QTabBar::tab:hover:!selected { background: @cardHover; color: @text; }
QTabBar::tab:selected {
    background: @accentSoft;
    color: @accent;
    font-weight: 600;
}

/* Hero strip on the "Installed" tab: a soft card, no shadow (QSS cannot). */
QFrame#startPageHeader {
    background: @surface;
    border: 1px solid @border;
    border-radius: 12px;
}

QPushButton {
    background: @surface;
    color: @text;
    border: 1px solid @borderStrong;
    border-radius: 8px;
    padding: 6px 14px;
    min-height: 18px;
}
QPushButton:hover { background: @cardHover; border-color: @muted; }
QPushButton:pressed { background: @surfaceAlt; }
QPushButton:disabled {
    background: @surfaceAlt;
    color: @faint;
    border-color: @border;
}
QPushButton[accent="true"] {
    background: @accentFill;
    color: @onAccent;
    border: 1px solid @accentFill;
    font-weight: 600;
}
QPushButton[accent="true"]:hover {
    background: @accentHover;
    border-color: @accentHover;
}
QPushButton[accent="true"]:pressed { background: @accentHover; }
QPushButton[accent="true"]:disabled {
    background: @accentSoft;
    border-color: @accentSoft;
    color: @faint;
}

QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 4px 8px;
    color: @muted;
}
QToolButton:hover { background: @cardHover; color: @text; }
QToolButton:checked { background: @accentSoft; color: @accent; }

QLineEdit, QPlainTextEdit, QTextEdit, QSpinBox, QDoubleSpinBox {
    background: @surface;
    color: @text;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 5px 8px;
    selection-background-color: @accentFill;
    selection-color: @onAccent;
}
QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus,
QSpinBox:focus, QDoubleSpinBox:focus {
    /* A 2 px accent ring: the 1 px hairline was practically invisible on the
       dark card and too faint in the light theme. */
    border: 2px solid @accent;
    padding: 4px 7px;
}
QSpinBox:focus, QDoubleSpinBox:focus { padding: 4px 21px 4px 7px; }
QLineEdit:disabled, QPlainTextEdit:disabled, QTextEdit:disabled,
QSpinBox:disabled, QDoubleSpinBox:disabled {
    background: @surfaceAlt;
    color: @faint;
}
QLineEdit[role="danger"], QPlainTextEdit[role="danger"], QTextEdit[role="danger"],
QSpinBox[role="danger"], QDoubleSpinBox[role="danger"] {
    color: @danger;
    border-color: @danger;
}

/* Spin boxes: the sheet replaces the native sub-controls, so both arrows are
   drawn from the SVG chevrons (muted grey, readable on both palettes). */
QSpinBox, QDoubleSpinBox { padding-right: 22px; }
QSpinBox::up-button, QDoubleSpinBox::up-button {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 18px;
    margin: 1px 1px 0 0;
    border: none;
    background: transparent;
}
QSpinBox::down-button, QDoubleSpinBox::down-button {
    subcontrol-origin: border;
    subcontrol-position: bottom right;
    width: 18px;
    margin: 0 1px 1px 0;
    border: none;
    background: transparent;
}
QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
    background: @cardHover;
    border-radius: 4px;
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow {
    image: url(:/icons/chevron-up.svg);
    width: 9px;
    height: 9px;
}
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow {
    image: url(:/icons/chevron-down.svg);
    width: 9px;
    height: 9px;
}

QComboBox {
    background: @surface;
    color: @text;
    border: 1px solid @border;
    border-radius: 8px;
    padding: 5px 8px;
    min-height: 18px;
}
QComboBox:hover { border-color: @borderStrong; }
QComboBox:focus { border: 2px solid @accent; padding: 4px 7px; }
QComboBox:disabled { background: @surfaceAlt; color: @faint; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox::down-arrow {
    image: url(:/icons/chevron-down.svg);
    width: 10px;
    height: 10px;
}
QComboBox QAbstractItemView {
    /* The popup must read as a layer above the card: a slightly different
       surface plus a stronger outline, otherwise the rows melt into the form.
       Row metrics are left to the style: overriding item padding/min-height
       made the popup clip two-row lists and show a scrollbar. */
    background: @popup;
    color: @text;
    border: 1px solid @borderStrong;
    border-radius: 8px;
    outline: none;
    selection-background-color: @selected;
    selection-color: @text;
}

QCheckBox, QRadioButton { color: @text; spacing: 8px; }
QCheckBox:disabled, QRadioButton:disabled { color: @faint; }

QSlider::groove:horizontal {
    height: 4px;
    background: @borderStrong;
    border-radius: 2px;
}
QSlider::sub-page:horizontal { background: @accent; border-radius: 2px; }
QSlider::handle:horizontal {
    background: @accentFill;
    width: 14px;
    margin: -5px 0;
    border-radius: 7px;
}
QSlider::handle:horizontal:hover { background: @accentHover; }

QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: @borderStrong;
    min-height: 28px;
    border-radius: 5px;
}
QScrollBar::handle:vertical:hover { background: @muted; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; border: none; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: @borderStrong;
    min-width: 28px;
    border-radius: 5px;
}
QScrollBar::handle:horizontal:hover { background: @muted; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; border: none; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }

QProgressBar {
    background: @surfaceAlt;
    color: @text;
    border: 1px solid @border;
    border-radius: 6px;
    height: 14px;
    text-align: center;
    font-size: 10px;
}
QProgressBar::chunk { background: @accentFill; border-radius: 5px; }

QProgressBar[thin="true"] {
    height: 6px;
    border: none;
    border-radius: 3px;
    background: @border;
}
QProgressBar[thin="true"]::chunk { background: @accentFill; border-radius: 3px; }
QProgressBar[thin="true"][level="warning"]::chunk { background: @warning; }
QProgressBar[thin="true"][level="danger"]::chunk { background: @danger; }

QGroupBox {
    background: @surface;
    border: 1px solid @border;
    border-radius: 12px;
    margin-top: 14px;
    padding-top: 10px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 12px;
    padding: 0 6px;
    color: @muted;
    font-weight: 600;
}

QSplitter::handle { background: @border; }
QSplitter::handle:horizontal { width: 2px; }
QSplitter::handle:vertical { height: 2px; }

QStatusBar {
    background: @bg;
    color: @muted;
    border-top: 1px solid @border;
}
QStatusBar::item { border: none; }

QHeaderView::section {
    background: @surfaceAlt;
    color: @muted;
    border: none;
    border-bottom: 1px solid @border;
    padding: 6px 8px;
}
QTableView, QTableWidget, QTreeView {
    background: @surface;
    alternate-background-color: @surfaceAlt;
    color: @text;
    border: 1px solid @border;
    border-radius: 8px;
    selection-background-color: @accentSoft;
    selection-color: @text;
}

/* Plain list widgets (hub browsers, pickers in dialogs): rounded rows with the
   soft accent as selection instead of the system blue. */
QListWidget, QListView {
    background: @surface;
    color: @text;
    border: 1px solid @border;
    border-radius: 8px;
    outline: none;
}
QListWidget::item, QListView::item {
    padding: 5px 8px;
    border-radius: 6px;
    color: @text;
}
QListWidget::item:hover, QListView::item:hover { background: @cardHover; }
QListWidget::item:selected, QListView::item:selected {
    background: @accentSoft;
    color: @text;
}
QListWidget::item:disabled, QListView::item:disabled { color: @faint; }

/* The repository cards paint themselves (CardDelegate): keep the item box
   untouched so the cards stay full-bleed and the delegate owns the corners. */
QListWidget#modelList, QListWidget#browserList {
    background: transparent;
    border: none;
}
QListWidget#modelList::item, QListWidget#browserList::item,
QListWidget#modelList::item:hover, QListWidget#browserList::item:hover,
QListWidget#modelList::item:selected, QListWidget#browserList::item:selected {
    padding: 0px;
    border-radius: 0px;
    background: transparent;
}
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget#qt_scrollarea_viewport { background: transparent; }
)QSS");

    css = sub(css, "@bg", p.bg);
    css = sub(css, "@surfaceAlt", p.surfaceAlt);
    css = sub(css, "@popup", p.popup);
    css = sub(css, "@surface", p.surface);
    css = sub(css, "@cardHover", p.cardHover);
    css = sub(css, "@accentSoft", p.accentSoft);
    css = sub(css, "@accentFill", p.accentFill);
    css = sub(css, "@accentHover", p.accentHover);
    css = sub(css, "@borderStrong", p.borderStrong);
    css = sub(css, "@border", p.border);
    css = sub(css, "@accent", p.accent);
    css = sub(css, "@onAccent", p.onAccent);
    css = sub(css, "@text", p.text);
    css = sub(css, "@muted", p.muted);
    css = sub(css, "@faint", p.faint);
    css = sub(css, "@danger", p.danger);
    css = sub(css, "@warning", p.warning);
    css = sub(css, "@success", p.success);
    return css;
}

void apply()
{
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app)
        return; // headless run (--no-tray): no widgets, nothing to style
    if (!app->style()
        || app->style()->objectName().compare(QLatin1String("fusion"), Qt::CaseInsensitive) != 0) {
        if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
            app->setStyle(fusion);
    }
    Theme::instance()->install();
}

} // namespace theme
