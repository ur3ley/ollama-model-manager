#ifndef HFCLIENT_H
#define HFCLIENT_H

#include <QObject>
#include <QList>
#include <QString>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <memory>

// Thin wrapper over the public HuggingFace Hub API, enough to browse GGUF
// repos and download a single quant file.
//
// Endpoints used:
//   GET /api/models?search=<q>&filter=gguf&sort=downloads
//   GET /api/models/<repo>?blobs=true
//   GET /<repo>/resolve/main/<file>
//
// Public repositories need no authentication; gated ones answer 401 until a
// user token is set with setToken(). The token is sent as
// `Authorization: Bearer <token>` on every request to the Hub, exactly what
// the Hub asks for — and never to another host a redirect points at (the
// `/resolve/` CDN hop is followed by hand without it).
//
// The JSON parsers and the request builder are static so they can be tested
// offline.
class HfClient : public QObject
{
    Q_OBJECT
public:
    struct Repo {
        QString id;
        qint64 downloads = 0;
        qint64 likes = 0;
        QString sha;           // current commit, used for update checks
        QString lastModified;  // ISO-8601
        QStringList tags;      // HF tags (text-generation, image-text-to-text, ...)
    };

    struct File {
        QString name;     // file name inside the repo
        qint64 size = 0;  // bytes
        bool split = false;
        int part = 0;     // 1-based for split files
        int parts = 0;
        QString quant;    // Q4_K_M, UD-Q4_K_XL, IQ4_XS, ...

        bool isGguf() const { return name.endsWith(QLatin1String(".gguf"), Qt::CaseInsensitive); }
    };

    explicit HfClient(QObject *parent = nullptr);
    ~HfClient() override;

    /**
     * User token for gated repositories. Empty (the default) means anonymous
     * access. The value is trimmed, so a token pasted with a trailing newline
     * still produces a valid header.
     */
    void setToken(const QString &token);
    QString token() const { return m_token; }
    bool hasToken() const { return !m_token.isEmpty(); }

    /** Trim spaces/line breaks from a pasted token (static for the self-test). */
    static QString normalizeToken(const QString &token);
    /** Is this error a Hub "gated repository" rejection? */
    static bool isAuthError(const QString &error);
    /**
     * Request builder used by get(); static and side-effect free so the
     * Authorization header can be checked without any network access.
     */
    static QNetworkRequest requestFor(const QUrl &url, const QString &token);
    /**
     * Request for the next hop of a manually followed redirect: same origin
     * keeps every header, another host gets them all except Authorization.
     */
    static QNetworkRequest redirectRequestFor(const QNetworkRequest &previous, const QUrl &target);
    /** Same scheme, host (case-insensitive) and effective port. */
    static bool sameOrigin(const QUrl &a, const QUrl &b);

    void search(const QString &query, int limit = 40,
                const QString &sort = QStringLiteral("downloads"));
    void listFiles(const QString &repo);
    /** Current commit sha of a repository (update check). */
    void fetchRepoInfo(const QString &repo);
    /**
     * Download one repository file into `destDir` (resuming a `.part`).
     * `expectedSize` (bytes, 0 = unknown) is the size the Hub API reported:
     * a body of a different length is rejected instead of being renamed into
     * a GGUF that `ollama create` would import truncated.
     * `revision` (a commit sha; empty = `main`) pins the download to one
     * commit, also across resumes: a `.part` recorded for another revision
     * is started over instead of being spliced (B-10).
     */
    void download(const QString &repo, const QString &file, const QString &destDir,
                  qint64 expectedSize = 0, const QString &revision = QString());
    /** Tests only: talk to `base` (e.g. http://127.0.0.1:port) instead of huggingface.co. */
    static void setHubBaseForTest(const QString &base);
    void cancelDownload();

    static QList<Repo> parseSearch(const QByteArray &json);
    static QList<File> parseFiles(const QByteArray &json);
    static File classifyFile(const QString &name, qint64 size);
    static QString modelNameFor(const QString &repo, const QString &fileName);

signals:
    void searchFinished(bool ok, const QList<HfClient::Repo> &repos, const QString &error);
    void filesFinished(bool ok, const QList<HfClient::File> &files, const QString &error);
    void repoInfoFinished(const QString &repo, const QString &sha, bool ok, const QString &error);
    void downloadProgress(qint64 received, qint64 total);
    void downloadFinished(bool ok, const QString &path, const QString &error);

private:
    // One download across its redirect hops (defined in hfclient.cpp).
    struct DownloadState;

    QNetworkReply *get(const QUrl &url);
    void startDownloadRequest(const std::shared_ptr<DownloadState> &state,
                              const QNetworkRequest &request);
    static QString networkError(QNetworkReply *reply);

    QString m_token;  // empty: anonymous Hub access
    QNetworkAccessManager *m_nam;
    QNetworkReply *m_download = nullptr;
    QNetworkReply *m_search = nullptr;
    QNetworkReply *m_files = nullptr;
    QNetworkReply *m_repoInfo = nullptr;
};

#endif // HFCLIENT_H
