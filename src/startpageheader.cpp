#include "startpageheader.h"

#include "designtokens.h"
#include "doticons.h"
#include "i18n.h"
#include "memoryinfo.h"
#include "ollamacli.h"
#include "theme.h"

#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// The hero refreshes itself: the RAM line and the Ollama dot every second, the
// disk line every fifth tick (QStorageInfo can block on a network mount).
constexpr int kTickMs = 1000;
constexpr int kDiskEveryTicks = 5;

// render() runs on that timer too, and setText() forces a relayout, so strings
// are only assigned when they really changed.
void setLabelText(QLabel *label, const QString &text)
{
    if (label->text() != text)
        label->setText(text);
}

// Same idea for the semantic colour: repolishing a label is not free.
void setLabelRole(QWidget *widget, const char *role)
{
    const QString value = role && *role ? QString::fromLatin1(role) : QString();
    if (widget->property("role").toString() == value)
        return;
    tokens::setRole(widget, role);
}

// OLLAMA_HOST may be "host:port", "http://host:port" or a unix socket.
// The hero only reports the port, the default one when nothing is configured.
QString ollamaPort()
{
    const QString host = qEnvironmentVariable("OLLAMA_HOST").trimmed();
    if (host.isEmpty())
        return QStringLiteral("11434");
    QString value = host;
    const int scheme = value.indexOf(QStringLiteral("://"));
    if (scheme >= 0)
        value = value.mid(scheme + 3);
    const int slash = value.indexOf(QLatin1Char('/'));
    if (slash >= 0)
        value.truncate(slash);
    const int colon = value.lastIndexOf(QLatin1Char(':'));
    if (colon >= 0 && colon + 1 < value.size())
        return value.mid(colon + 1);
    return QStringLiteral("11434");
}

// "1.4 ТБ" / "128 ГБ": one decimal below 10, none above; units come from the
// language file.
QString sizeText(quint64 bytes)
{
    constexpr double gib = 1024.0 * 1024.0 * 1024.0;
    constexpr double tib = gib * 1024.0;
    if (double(bytes) >= tib)
        return QString::number(double(bytes) / tib, 'f', 1) + QLatin1Char(' ')
                + T("hero.unitTb");
    const double value = double(bytes) / gib;
    return QString::number(value, 'f', value >= 10.0 ? 0 : 1) + QLatin1Char(' ')
            + T("hero.unitGb");
}

} // namespace

// Label + thin bar: one compact metric for the hero row.
class UsageMeter : public QWidget
{
public:
    explicit UsageMeter(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        m_label = new QLabel();
        QFont font = m_label->font();
        font.setPixelSize(12);
        m_label->setFont(font);
        tokens::setMuted(m_label);
        layout->addWidget(m_label);

        m_bar = new QProgressBar();
        m_bar->setRange(0, 100);
        m_bar->setTextVisible(false);
        m_bar->setFixedHeight(6);
        m_bar->setMinimumWidth(150);
        m_bar->setMaximumWidth(220);
        m_bar->setProperty("thin", true);
        m_bar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        layout->addWidget(m_bar);
    }

    void setText(const QString &text)
    {
        if (m_label->text() == text)
            return;
        m_label->setText(text);
    }

    void setPercent(int percent)
    {
        m_bar->setValue(qBound(0, percent, 100));
        const char *level = percent >= 97 ? "danger" : percent >= 90 ? "warning" : "";
        if (m_bar->property("level").toString() == QLatin1String(level))
            return;
        m_bar->setProperty("level", QString::fromLatin1(level));
        m_bar->style()->unpolish(m_bar);
        m_bar->style()->polish(m_bar);
    }

private:
    QLabel *m_label;
    QProgressBar *m_bar;
};

StartPageHeader::StartPageHeader(OllamaCli *cli, QWidget *parent)
    : QFrame(parent)
    , m_cli(cli)
{
    setObjectName(QStringLiteral("startPageHeader"));
    // A bare QFrame does not paint a QSS background unless it is told to.
    setAttribute(Qt::WA_StyledBackground, true);
    setMinimumHeight(96);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(20, 16, 20, 16);
    root->setSpacing(16);

    auto *left = new QVBoxLayout();
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(8);

    m_title = new QLabel();
    QFont titleFont = m_title->font();
    titleFont.setPixelSize(20);
    titleFont.setWeight(QFont::DemiBold);
    m_title->setFont(titleFont);
    left->addWidget(m_title);

    m_hint = new QLabel();
    QFont captionFont = m_hint->font();
    captionFont.setPixelSize(13);
    m_hint->setFont(captionFont);
    tokens::setMuted(m_hint);
    left->addWidget(m_hint);

    m_metrics = new QWidget();
    auto *metricsRow = new QHBoxLayout(m_metrics);
    metricsRow->setContentsMargins(0, 0, 0, 0);
    metricsRow->setSpacing(24);

    m_ramMeter = new UsageMeter();
    metricsRow->addWidget(m_ramMeter, 0, Qt::AlignVCenter);

    m_diskMeter = new UsageMeter();
    metricsRow->addWidget(m_diskMeter, 0, Qt::AlignVCenter);

    m_statusDot = new QLabel();
    m_statusDot->setFixedSize(10, 10);
    metricsRow->addWidget(m_statusDot, 0, Qt::AlignVCenter);

    m_statusText = new QLabel();
    m_statusText->setFont(captionFont);
    tokens::setMuted(m_statusText);
    metricsRow->addWidget(m_statusText, 0, Qt::AlignVCenter);
    metricsRow->addStretch(1);

    left->addWidget(m_metrics);
    root->addLayout(left, 1);

    m_downloadBtn = new QPushButton();
    m_downloadBtn->setProperty("accent", true);
    root->addWidget(m_downloadBtn, 0, Qt::AlignVCenter);

    connect(m_downloadBtn, &QPushButton::clicked, this, &StartPageHeader::downloadRequested);

    // The palette is applied by the global sheet; only the status dot is a
    // pixmap and has to be repainted by hand.
    connect(theme::Theme::instance(), &theme::Theme::changed, this, [this]() {
        m_statusDot->setPixmap(dotIcon(m_ollamaOk).pixmap(10, 10));
    });
    connect(I18n::instance(), &I18n::languageChanged, this, &StartPageHeader::retranslateUi);

    refreshStatus();
    refreshDisk();
    refreshMemory();
    retranslateUi();

    // RAM/disk/status drift while the window just sits there; the model list
    // itself is only reloaded on events, never from here.
    m_tick = new QTimer(this);
    m_tick->setInterval(kTickMs);
    connect(m_tick, &QTimer::timeout, this, &StartPageHeader::tick);
    m_tick->start();
}

void StartPageHeader::showEvent(QShowEvent *event)
{
    QFrame::showEvent(event);
    // The timer idles while the strip is hidden (another tab is open), so catch
    // up immediately. The disk counter is left alone on purpose: resetting it
    // here would flip the disk line on every tab switch.
    refreshMemory();
    refreshStatus();
    render();
}

void StartPageHeader::tick()
{
    // Hidden means the installed tab is not current; skip the reads entirely.
    if (!isVisible())
        return;
    refreshMemory();
    refreshStatus();
    if (++m_diskTicks >= kDiskEveryTicks) {
        m_diskTicks = 0;
        refreshDisk();
    }
    render();
}

void StartPageHeader::updateSummary(const InstalledSummary &summary)
{
    m_summary = summary;
    refreshStatus();
    refreshDisk();
    refreshMemory();
    render();
}

void StartPageHeader::retranslateUi()
{
    render();
}

void StartPageHeader::refreshStatus()
{
    // Look the binary up again on every refresh: a reinstall or a moved
    // `ollama` must light the dot without waiting for the next modelsLoaded.
    // Purely local — no network probe, the port is only reported in the tooltip.
    QString path = QStandardPaths::findExecutable(QStringLiteral("ollama"));
    if (path.isEmpty() && m_cli)
        path = m_cli->ollamaPath();
    // OllamaCli keeps a bare "ollama" when the lookup failed; treat only a real
    // executable as found, otherwise the hero would always claim a healthy CLI.
    m_ollamaOk = !path.isEmpty() && QFileInfo(path).isExecutable();
    m_ollamaPath = path;
}

void StartPageHeader::refreshDisk()
{
    const QDir home(QDir::homePath());
    QString path = home.filePath(QStringLiteral(".ollama/models"));
    if (!QFileInfo::exists(path))
        path = home.path();
    const QStorageInfo info(path);
    const bool known = info.isValid() && info.isReady();
    m_diskTotalBytes = known ? quint64(info.bytesTotal()) : 0;
    m_diskUsedBytes = known ? quint64(info.bytesTotal() - info.bytesAvailable()) : 0;
}

void StartPageHeader::refreshMemory()
{
    m_ramTotalBytes = memoryinfo::memTotalBytes();
    const quint64 available = memoryinfo::memAvailableBytes();
    m_ramUsedBytes = m_ramTotalBytes > available ? m_ramTotalBytes - available : 0;
}

void StartPageHeader::render()
{
    const bool empty = m_summary.quants <= 0;
    const bool showMetrics = !empty;

    m_metrics->setVisible(showMetrics);
    m_hint->setVisible(empty);
    m_downloadBtn->setVisible(empty);
    const QString cta = T("hero.emptyCta");
    if (m_downloadBtn->text() != cta)
        m_downloadBtn->setText(cta);

    if (empty) {
        setLabelText(m_title, T("hero.emptyTitle"));
        m_title->setToolTip(QString());
        m_statusDot->setPixmap(dotIcon(m_ollamaOk).pixmap(10, 10));
        setLabelText(m_hint, m_ollamaOk ? T("hero.emptyHint") : T("hero.ollamaMissing"));
        setLabelRole(m_hint, m_ollamaOk ? nullptr : "danger");
        return;
    }

    const QString count = QString::number(m_summary.quants);
    const QString size = sizeText(m_summary.bytes);
    setLabelText(m_title,
                 (m_summary.quants == 1 ? T("hero.modelsOne") : T("hero.models"))
                         .arg(count, size));
    m_title->setToolTip(T("hero.summaryTip").arg(count, QString::number(m_summary.repositories)));

    const auto percent = [](quint64 used, quint64 total) {
        return total > 0 ? int(qMin<quint64>(100, used * 100 / total)) : 0;
    };

    if (m_ramTotalBytes > 0) {
        m_ramMeter->setVisible(true);
        m_ramMeter->setText(T("hero.ram")
                                    .arg(sizeText(m_ramUsedBytes),
                                         sizeText(m_ramTotalBytes)));
        m_ramMeter->setPercent(percent(m_ramUsedBytes, m_ramTotalBytes));
    } else {
        m_ramMeter->setVisible(false);
    }

    if (m_diskTotalBytes > 0) {
        m_diskMeter->setVisible(true);
        m_diskMeter->setText(T("hero.disk")
                                     .arg(sizeText(m_diskUsedBytes),
                                          sizeText(m_diskTotalBytes)));
        m_diskMeter->setPercent(percent(m_diskUsedBytes, m_diskTotalBytes));
    } else {
        m_diskMeter->setVisible(false);
    }

    m_statusDot->setPixmap(dotIcon(m_ollamaOk).pixmap(10, 10));
    setLabelText(m_statusText, m_ollamaOk ? T("hero.ollamaLabel") : T("hero.ollamaMissing"));
    setLabelRole(m_statusText, m_ollamaOk ? nullptr : "danger");
    m_statusText->setToolTip(m_ollamaOk
                                     ? T("hero.ollamaOk").arg(m_ollamaPath, ollamaPort())
                                     : T("hero.ollamaMissing"));
}
