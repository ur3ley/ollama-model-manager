#ifndef PULLPROGRESS_H
#define PULLPROGRESS_H

#include <QHash>
#include <QString>

namespace pullprogress {

/**
 * "522 MB", "1.2 GB", "512 KB", "45 B" → bytes using decimal (SI) units.
 * `ollama` prints sizes with 1000-based units (`522 MB` is 522'000'000 bytes,
 * the manifest says 522'653'277), so the byte budget must be counted the same
 * way or every layer is ~5% short.
 */
qint64 parseSizeBytes(const QString &text);

} // namespace pullprogress

// Aggregates `ollama pull` CLI output into one overall progress value.
//
// `ollama pull` downloads several layers one after another and every layer
// prints its own "pulling <id>:  45% ▕…▏ 1.2 GB/1.5 GB" line. Reading the
// percentage straight off those lines makes the bar jump back to 0 for each new
// layer (a 39 GB model sat at "100% — 900 MB / 902 MB"), so the parser keeps the
// byte counters per layer and reports their sum against the expected total.
// Pure text processing: no widgets, no processes.
class PullProgressParser
{
public:
    struct Update {
        qint64 received = 0;  // bytes downloaded so far, all layers
        qint64 total = 0;     // expected bytes, 0 when unknown
        int percent = -1;     // -1 when total is unknown
        QString label;        // short status for the UI
    };

    /** Forget every layer and the expected size. */
    void reset();

    /** Known download size (registry tag size); 0 means "sum the layers". */
    void setExpectedBytes(qint64 bytes);

    /**
     * Feed one output line (may still contain ANSI escapes, \r redraws and
     * several frames glued with cursor moves).
     * Returns the aggregated state after the line; lines that carry no new
     * information just return the current state.
     */
    Update feed(const QString &line);

    /**
     * `ollama pull` exits 0: the bar must read 100% even if the last frame of
     * the biggest layer was never printed (Ollama redraws over it).
     */
    Update markComplete();

private:
    struct Layer {
        qint64 received = 0;
        qint64 total = 0;
        bool done = false;
    };

    Update snapshot() const;

    qint64 m_expectedBytes = 0;
    QHash<QString, Layer> m_layers;
    QString m_label;
    bool m_complete = false;
};

#endif // PULLPROGRESS_H
