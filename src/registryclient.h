#ifndef REGISTRYCLIENT_H
#define REGISTRYCLIENT_H

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

    explicit RegistryClient(QObject *parent = nullptr);

    void search(const QString &query);
    void listTags(const QString &family);

    /** Fetch the registry manifest digest for "family:tag" (update check). */
    void checkManifest(const QString &ref);

    static QList<Model> parseSearch(const QString &html);
    static QList<Tag> parseTags(const QString &html);

signals:
    void searchFinished(bool ok, const QList<RegistryClient::Model> &models, const QString &error);
    void tagsFinished(bool ok, const QList<RegistryClient::Tag> &tags, const QString &error);
    void manifestChecked(const QString &ref, const QString &digest, bool ok, const QString &error);

private:
    QNetworkReply *get(const QString &url);
    QNetworkAccessManager *m_nam;
    QNetworkReply *m_search = nullptr;
    QNetworkReply *m_tags = nullptr;
    QNetworkReply *m_manifest = nullptr;
};

#endif // REGISTRYCLIENT_H
