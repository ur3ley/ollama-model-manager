#ifndef REGISTRYCLIENT_H
#define REGISTRYCLIENT_H

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

// Browses the public Ollama library (ollama.com).
//
// There is no JSON API for search or tag lists — `/api/search` returns 404 and
// `registry.ollama.ai/v2/.../tags/list` is unsupported — so both pages are
// parsed from HTML. Parsing is isolated in static functions and covered by an
// offline self-test, so a markup change shows up as a failing test instead of
// an empty window.
class RegistryClient : public QObject
{
    Q_OBJECT
public:
    struct Model {
        QString name;
        QString description;
        QStringList capabilities;  // tools, thinking, vision, embedding
        QStringList sizes;         // 0.6b, 8b, 235b ...
        QString pulls;             // 121.3M
    };

    struct Tag {
        QString name;     // family:tag
        QString size;     // 5.2GB
        QString context;  // 40K
        QString input;    // Text / Vision
    };

    // What the library page of a family says about the model itself
    // (`/library/<family>`): the long blurb, the readme source and the
    // "Updated … ago" line from the header.
    struct ModelDetails {
        QString description;     // <meta property="og:description">
        QString readmeMarkdown;  // raw markdown, empty when the page has none
        QString updated;         // "1 year ago"
        QStringList capabilities;
        QStringList sizes;
    };

    // What a single tag page (`/library/<family>:<tag>`) says about the
    // weights: architecture, parameter count and quantization live in the
    // `model` blob row, the license text in the `license` blob row.
    struct TagDetails {
        QString architecture;  // qwen3moe
        QString parameters;    // 30.5B
        QString quantization;  // Q4_K_M
        QString license;       // Apache License
    };

    explicit RegistryClient(QObject *parent = nullptr);

    void search(const QString &query);
    void listTags(const QString &family);

    /** Family page details (description, readme, update age). Cached per session. */
    void fetchModelDetails(const QString &family);
    /** Tag page details (arch/parameters/quantization/license) for "family:tag". */
    void fetchTagDetails(const QString &ref);

    /** Fetch the registry manifest digest for "family:tag" (update check). */
    void checkManifest(const QString &ref);

    static QList<Model> parseSearch(const QString &html);
    static QList<Tag> parseTags(const QString &html);
    static ModelDetails parseModelPage(const QString &html);
    static TagDetails parseTagPage(const QString &html);

signals:
    void searchFinished(bool ok, const QList<RegistryClient::Model> &models, const QString &error);
    void tagsFinished(bool ok, const QList<RegistryClient::Tag> &tags, const QString &error);
    void modelDetailsReady(const QString &family, const RegistryClient::ModelDetails &details,
                           bool ok);
    void tagDetailsReady(const QString &ref, const RegistryClient::TagDetails &details, bool ok);
    void manifestChecked(const QString &ref, const QString &digest, bool ok, const QString &error);

private:
    QNetworkReply *get(const QString &url);
    QNetworkAccessManager *m_nam;
    QNetworkReply *m_search = nullptr;
    QNetworkReply *m_tags = nullptr;
    QNetworkReply *m_modelDetails = nullptr;
    QNetworkReply *m_tagDetails = nullptr;
    QNetworkReply *m_manifest = nullptr;

    // Page data does not change during a session; a cache hit skips the round
    // trip and emits the signal right away.
    QHash<QString, ModelDetails> m_modelCache;
    QHash<QString, TagDetails> m_tagCache;
};

#endif // REGISTRYCLIENT_H
