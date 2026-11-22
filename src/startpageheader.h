#ifndef STARTPAGEHEADER_H
#define STARTPAGEHEADER_H

#include <QFrame>
#include <QString>

#include "modellistwidget.h"

class OllamaCli;
class QLabel;
class QPushButton;
class QShowEvent;
class QTimer;
class UsageMeter;

// Hero strip above the installed-models splitter: a one-glance answer to
// "what do I have here". The big line shows how many quanta are installed and
// how much they take; next to it are RAM and disk meters and the Ollama status.
// With zero models it turns into an empty state with a single primary CTA.
// Every colour comes from the active palette (theme QSS), every string from
// languages/*.ini.
class StartPageHeader : public QFrame
{
    Q_OBJECT
public:
    explicit StartPageHeader(OllamaCli *cli, QWidget *parent = nullptr);

    /** Totals from ModelListWidget::summary(); meters are re-read. */
    void updateSummary(InstalledSummary summary);

signals:
    void downloadRequested();

protected:
    void showEvent(QShowEvent *event) override;

private:
    /** One timer tick: the cheap numbers every second, the disk every fifth. */
    void tick();
    void retranslateUi();
    void refreshStatus();
    void refreshDisk();
    void refreshMemory();
    /** Rebuild every string from the stored numbers (used by language switch). */
    void render();
    /** Binary found and the last `ollama list` reached the daemon. */
    bool ollamaHealthy() const;
    /** "not found" or "not responding", whichever applies. */
    QString ollamaProblem() const;

    OllamaCli *m_cli;
    QLabel *m_title;
    QLabel *m_hint;
    QLabel *m_statusDot;
    QLabel *m_statusText;
    QWidget *m_metrics;
    UsageMeter *m_ramMeter;
    UsageMeter *m_diskMeter;
    QPushButton *m_downloadBtn;
    QTimer *m_tick = nullptr;
    int m_diskTicks = 0;
    int m_statusTicks = 0;

    InstalledSummary m_summary;
    bool m_ollamaOk = false;       // binary found
    bool m_daemonOk = true;        // last `ollama list` succeeded (unknown = ok)
    QString m_daemonError;
    QString m_ollamaPath;
    quint64 m_diskUsedBytes = 0;
    quint64 m_diskTotalBytes = 0;
    quint64 m_ramUsedBytes = 0;
    quint64 m_ramTotalBytes = 0;
};

#endif // STARTPAGEHEADER_H
