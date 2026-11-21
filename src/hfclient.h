#ifndef HFCLIENT_H
#define HFCLIENT_H

#include <QObject>
#include <QList>
#include <QString>

#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

// Thin wrapper over the public HuggingFace Hub API, enough to browse GGUF
// repos and download a single quant file.
//
// Only public endpoints are used (no token):
//   GET /api/models?search=<q>&filter=gguf&sort=downloads
//   GET /api/models/<repo>?blobs=true
//   GET /<repo>/resolve/main/<file>
//
// The JSON parsers are static so they can be tested offline.
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

    void search(const QString &query, int limit = 40,
                const QString &sort = QStringLiteral("downloads"));
    void listFiles(const QString &repo);
    /** Current commit sha of a repository (update check). */
    void fetchRepoInfo(const QString &repo);
    void download(const QString &repo, const QString &file, const QString &destDir);
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
    QNetworkReply *get(const QUrl &url);
    static QString networkError(QNetworkReply *reply);

    QNetworkAccessManager *m_nam;
    QNetworkReply *m_download = nullptr;
    QNetworkReply *m_search = nullptr;
    QNetworkReply *m_files = nullptr;
    QNetworkReply *m_repoInfo = nullptr;
};

#endif // HFCLIENT_H
