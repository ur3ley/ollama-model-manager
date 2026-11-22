#include "hfclient.h"

#include "quantmatch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtNetwork/QNetworkRequest>
#include <QUrlQuery>

namespace {

const char *kHubBase = "https://huggingface.co";
QString g_hubBaseOverride;  // tests only: a local HTTP server instead of the Hub

QString hubBase()
{
    return g_hubBaseOverride.isEmpty() ? QString::fromLatin1(kHubBase) : g_hubBaseOverride;
}

// Revision a `.part` was downloaded from: a resume must never append bytes of
// another commit to it (B-10).
QString revisionPath(const QString &partPath)
{
    return partPath + QStringLiteral(".rev");
}

QRegularExpression splitRe()
{
    static const QRegularExpression re(
        QStringLiteral("^(.*)-(\\d{5})-of-(\\d{5})\\.gguf$"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

QRegularExpression quantRe()
{
    // Longest alternatives first: UD-Q4_K_XL before Q4_K_XL, IQ4_XS after IQ4.
    static const QRegularExpression re(QStringLiteral(
        "(UD-[A-Za-z0-9_]+"
        "|IQ\\d+_[A-Z0-9]+"
        "|TQ\\d+_\\d+"
        "|Q\\d+_K_[A-Z]+"
        "|Q\\d+_\\d+_\\d+"
        "|Q\\d+_\\d+"
        "|BF16|F16|F32)"));
    return re;
}

// See RegistryClient: aborting a finished reply emits `finished` synchronously.
// No bytes for this long aborts a download (QNetworkRequest transfer timeout).
constexpr int kDownloadStallMs = 60000;
// Marks a reply aborted by cancelDownload(), as opposed to the stall timeout.
constexpr char kUserCancelProperty[] = "ommUserCancel";

QNetworkReply *disposeReply(QNetworkReply *&member, QObject *owner)
{
    QNetworkReply *reply = member;
    member = nullptr;
    if (!reply)
        return nullptr;
    reply->disconnect(owner);
    reply->abort();
    reply->deleteLater();
    return reply;
}

} // namespace

struct HfClient::DownloadState {
    QFile *out = nullptr;      // the `.part`, owned by HfClient until finished
    qint64 offset = 0;         // bytes already on disk when the body started
    bool probed = false;       // first data chunk seen (Range honoured or not)
    QString writeError;        // first failed write: abort, never rename
    QString finalPath;
    QString partPath;
    qint64 expectedSize = 0;
    int redirectsLeft = 10;
};

HfClient::HfClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

HfClient::~HfClient()
{
    for (QNetworkReply *reply : {m_download, m_search, m_files}) {
        if (reply)
            reply->abort();
    }
}

QNetworkReply *HfClient::get(const QUrl &url)
{
    return m_nam->get(requestFor(url, m_token));
}

QString HfClient::normalizeToken(const QString &token)
{
    // Tokens are copied from the Hub settings page; a trailing newline or a
    // stray space must never end up inside the Authorization header.
    return token.trimmed();
}

void HfClient::setToken(const QString &token)
{
    m_token = normalizeToken(token);
}

bool HfClient::isAuthError(const QString &error)
{
    // networkError() below prefixes every 401 with this marker.
    return error.startsWith(QLatin1String("HTTP 401"));
}

QNetworkRequest HfClient::requestFor(const QUrl &url, const QString &token)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ollama-model-manager"));
    const QString value = normalizeToken(token);
    // S-2: Qt copies every header, Authorization included, to the redirect
    // target. Anonymous requests may follow any https redirect; with a token
    // only same-origin ones (API redirects of renamed repos stay on the Hub).
    // Downloads, which do hop to the CDN, handle redirects by hand.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         value.isEmpty() ? QNetworkRequest::NoLessSafeRedirectPolicy
                                         : QNetworkRequest::SameOriginRedirectPolicy);
    if (!value.isEmpty())
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + value.toUtf8());
    return request;
}

bool HfClient::sameOrigin(const QUrl &a, const QUrl &b)
{
    const auto defaultPort = [](const QUrl &url) {
        return url.port(url.scheme() == QLatin1String("http") ? 80 : 443);
    };
    return a.scheme() == b.scheme()
            && a.host().compare(b.host(), Qt::CaseInsensitive) == 0
            && defaultPort(a) == defaultPort(b);
}

QNetworkRequest HfClient::redirectRequestFor(const QNetworkRequest &previous, const QUrl &target)
{
    if (sameOrigin(previous.url(), target)) {
        // Still the Hub: the token is needed and stays, redirects stay manual.
        QNetworkRequest next(previous);
        next.setUrl(target);
        return next;
    }
    // Another host (CDN, presigned storage URL): everything but the token.
    QNetworkRequest next(target);
    const QList<QByteArray> headers = previous.rawHeaderList();
    for (const QByteArray &name : headers) {
        if (name.compare("Authorization", Qt::CaseInsensitive) != 0)
            next.setRawHeader(name, previous.rawHeader(name));
    }
    next.setTransferTimeout(previous.transferTimeout());
    // Nothing secret is left to leak, so Qt may follow further hops itself.
    next.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                      QNetworkRequest::NoLessSafeRedirectPolicy);
    return next;
}

QString HfClient::networkError(QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::ContentNotFoundError)
        return QStringLiteral("HTTP 404: repository or file not found");
    if (reply->error() == QNetworkReply::AuthenticationRequiredError)
        return QStringLiteral("HTTP 401: this repository is gated, a token is required");
    return reply->errorString();
}

void HfClient::search(const QString &query, int limit, const QString &sort)
{
    disposeReply(m_search, this);

    QUrl url(hubBase() + QStringLiteral("/api/models"));
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("search"), query);
    params.addQueryItem(QStringLiteral("filter"), QStringLiteral("gguf"));
    params.addQueryItem(QStringLiteral("sort"), sort);
    params.addQueryItem(QStringLiteral("direction"), QStringLiteral("-1"));
    params.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    url.setQuery(params);

    QNetworkReply *reply = get(url);
    m_search = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_search == reply)
            m_search = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit searchFinished(false, {}, networkError(reply));
            return;
        }
        emit searchFinished(true, parseSearch(reply->readAll()), QString());
    });
}

void HfClient::listFiles(const QString &repo)
{
    disposeReply(m_files, this);

    QUrl url(hubBase() + QStringLiteral("/api/models/") + repo);
    url.setQuery(QStringLiteral("blobs=true"));
    QNetworkReply *reply = get(url);
    m_files = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_files == reply)
            m_files = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit filesFinished(false, {}, networkError(reply));
            return;
        }
        emit filesFinished(true, parseFiles(reply->readAll()), QString());
    });
}

void HfClient::setHubBaseForTest(const QString &base)
{
    g_hubBaseOverride = base;
}

void HfClient::download(const QString &repo, const QString &file, const QString &destDir,
                        qint64 expectedSize, const QString &revision)
{
    if (m_download)
        cancelDownload();
    disposeReply(m_download, this);

    if (!QDir().mkpath(destDir)) {
        emit downloadFinished(false, QString(),
                              QStringLiteral("cannot create directory ") + destDir);
        return;
    }

    // `file` is the repository-relative path (some repos keep quant folders
    // like `Q4_K_M/…`); the local copy uses the bare file name because all
    // download parts are flattened into `destDir` for the Modelfile wildcard.
    const QString fileName = QFileInfo(file).fileName();
    const QString finalPath = QDir(destDir).filePath(fileName);
    const QString partPath = finalPath + QStringLiteral(".part");
    // Resume support: a `.part` left by a crash/network failure is continued
    // with an HTTP range request instead of downloading tens of gigabytes
    // again. The final file appears only after the body was received whole, so
    // a leftover `.part` is never mistaken for a finished shard.
    qint64 alreadyOnDisk = QFileInfo::exists(partPath) ? QFileInfo(partPath).size() : 0;
    // A `.part` of another commit cannot be continued: start over. A `.part`
    // without a recorded revision (older versions) is resumed as before.
    const QString revFile = revisionPath(partPath);
    if (!revision.isEmpty()) {
        QFile rev(revFile);
        if (alreadyOnDisk > 0 && rev.open(QIODevice::ReadOnly)
                && QString::fromLatin1(rev.readAll()).trimmed() != revision)
            alreadyOnDisk = 0;
        rev.close();
        if (rev.open(QIODevice::WriteOnly | QIODevice::Truncate))
            rev.write(revision.toLatin1());
    }
    auto *out = new QFile(partPath, this);
    QIODevice::OpenMode openMode = QIODevice::WriteOnly;
    openMode |= alreadyOnDisk > 0 ? QIODevice::Append : QIODevice::Truncate;
    if (!out->open(openMode)) {
        const QString err = out->errorString();
        out->deleteLater();
        emit downloadFinished(false, QString(), err);
        return;
    }
    auto state = std::make_shared<DownloadState>();
    state->out = out;
    state->offset = alreadyOnDisk;
    state->finalPath = finalPath;
    state->partPath = partPath;
    state->expectedSize = expectedSize;

    const QUrl url(hubBase() + QLatin1Char('/') + repo
                   + QStringLiteral("/resolve/")
                   + (revision.isEmpty() ? QStringLiteral("main") : revision)
                   + QLatin1Char('/') + file);
    QNetworkRequest request = requestFor(url, m_token);
    // `/resolve/` answers 302 to a CDN host: with a token the hop is taken by
    // hand so the token never reaches it (S-2, see redirectRequestFor()).
    if (hasToken()) {
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::ManualRedirectPolicy);
    }
    // A stalled connection (no bytes for a minute) fails instead of hanging
    // forever; the `.part` is kept, so a retry resumes.
    request.setTransferTimeout(kDownloadStallMs);
    if (state->offset > 0) {
        request.setRawHeader("Range",
                             QByteArrayLiteral("bytes=")
                                     + QByteArray::number(state->offset) + QByteArrayLiteral("-"));
    }
    startDownloadRequest(state, request);
}

void HfClient::startDownloadRequest(const std::shared_ptr<DownloadState> &state,
                                    const QNetworkRequest &request)
{
    QNetworkReply *reply = m_nam->get(request);
    m_download = reply;

    connect(reply, &QNetworkReply::readyRead, this, [reply, state]() {
        // Only 200/206 carry file data; an error page (416, 401, 5xx) or a
        // redirect body must not reach the `.part` — finished() handles those.
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!state->writeError.isEmpty() || (status != 200 && status != 206)) {
            reply->readAll();
            return;
        }
        QFile *out = state->out;
        if (!state->probed) {
            state->probed = true;
            // A server that ignores Range answers 200 with the whole body: drop
            // the partial bytes we were going to append to.
            if (state->offset > 0 && status == 200) {
                out->close();
                if (out->open(QIODevice::WriteOnly | QIODevice::Truncate))
                    state->offset = 0;
            }
        }
        const QByteArray chunk = reply->readAll();
        if (out->write(chunk) != chunk.size()) {
            state->writeError = out->errorString();
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, reply, state](qint64 received, qint64 total) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status != 200 && status != 206)
            return;  // a redirect hop, not the file
        const qint64 done = state->offset + received;
        emit downloadProgress(done, total > 0 ? state->offset + total : -1);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, state]() {
        if (m_download == reply)
            m_download = nullptr;
        reply->deleteLater();

        QFile *out = state->out;
        const QString &finalPath = state->finalPath;
        const QString &partPath = state->partPath;
        const qint64 expectedSize = state->expectedSize;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        // Manual redirect (token requests only): follow it ourselves, keeping
        // the token on the Hub and dropping it for any other host.
        const QUrl location = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
        if (state->writeError.isEmpty() && reply->error() == QNetworkReply::NoError
            && status >= 300 && status < 400 && location.isValid()) {
            const QUrl target = reply->url().resolved(location);
            QString redirectError;
            if (--state->redirectsLeft < 0)
                redirectError = QStringLiteral("too many redirects");
            else if (reply->url().scheme() == QLatin1String("https")
                     && target.scheme() != QLatin1String("https"))
                redirectError = QStringLiteral("insecure redirect to ") + target.toString();
            if (redirectError.isEmpty()) {
                startDownloadRequest(state, redirectRequestFor(reply->request(), target));
                return;
            }
            out->close();
            out->deleteLater();
            emit downloadFinished(false, QString(), redirectError);
            return;
        }

        if (state->writeError.isEmpty() && reply->error() == QNetworkReply::NoError) {
            // Anything still buffered in the reply after the last readyRead.
            const QByteArray tail = reply->readAll();
            if (out->write(tail) != tail.size() || !out->flush())
                state->writeError = out->errorString();
        }
        out->close();
        out->deleteLater();

        if (!state->writeError.isEmpty()) {
            // Keep the `.part`: what reached the disk is a valid prefix.
            emit downloadFinished(false, QString(),
                                  QStringLiteral("cannot write ") + partPath
                                          + QStringLiteral(": ") + state->writeError);
            return;
        }

        if (status == 416) {
            // Range past the end: the `.part` is already complete — or longer
            // than the file (another revision). Only a matching size is kept.
            if (expectedSize > 0 && QFileInfo(partPath).size() == expectedSize) {
                QFile::remove(finalPath);
                if (QFile::rename(partPath, finalPath)) {
                    QFile::remove(revisionPath(partPath));
                    emit downloadFinished(true, finalPath, QString());
                    return;
                }
                emit downloadFinished(false, QString(),
                                      QStringLiteral("cannot rename ") + partPath);
                return;
            }
            QFile::remove(partPath);
            QFile::remove(revisionPath(partPath));
            emit downloadFinished(false, QString(),
                                  QStringLiteral("HTTP 416: the partial file did not match "
                                                 "and was discarded, retry to download again"));
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            // Keep the `.part`: the next attempt resumes from its length.
            // A stall timeout also reports OperationCanceledError; only an
            // abort through cancelDownload() is a user cancel.
            const bool cancelled = reply->error() == QNetworkReply::OperationCanceledError
                    && reply->property(kUserCancelProperty).toBool();
            const bool stalled = reply->error() == QNetworkReply::OperationCanceledError
                    && !cancelled;
            emit downloadFinished(false, QString(),
                                  cancelled ? QStringLiteral("cancelled")
                                  : stalled ? QStringLiteral("download stalled: no data for %1 s")
                                                      .arg(kDownloadStallMs / 1000)
                                            : networkError(reply));
            return;
        }

        if (expectedSize > 0) {
            const qint64 got = QFileInfo(partPath).size();
            if (got != expectedSize) {
                // Longer than announced cannot be resumed into anything valid.
                if (got > expectedSize) {
                    QFile::remove(partPath);
                    QFile::remove(revisionPath(partPath));
                }
                emit downloadFinished(false, QString(),
                                      QStringLiteral("size mismatch: got %1 of %2 bytes")
                                              .arg(got).arg(expectedSize));
                return;
            }
        }

        QFile::remove(finalPath);
        if (!QFile::rename(partPath, finalPath)) {
            emit downloadFinished(false, QString(),
                                  QStringLiteral("cannot rename ") + partPath);
            return;
        }
        QFile::remove(revisionPath(partPath));
        emit downloadFinished(true, finalPath, QString());
    });
}

void HfClient::cancelDownload()
{
    if (m_download) {
        m_download->setProperty(kUserCancelProperty, true);
        m_download->abort();
    }
}

QList<HfClient::Repo> HfClient::parseSearch(const QByteArray &json)
{
    QList<Repo> repos;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray())
        return repos;

    const QJsonArray items = doc.array();
    for (const QJsonValue &value : items) {
        const QJsonObject obj = value.toObject();
        const QString id = obj.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        Repo repo;
        repo.id = id;
        repo.downloads = qint64(obj.value(QStringLiteral("downloads")).toDouble());
        repo.likes = qint64(obj.value(QStringLiteral("likes")).toDouble());
        repo.sha = obj.value(QStringLiteral("sha")).toString();
        repo.lastModified = obj.value(QStringLiteral("lastModified")).toString();
        const QJsonArray tags = obj.value(QStringLiteral("tags")).toArray();
        for (const QJsonValue &tag : tags)
            repo.tags.append(tag.toString());
        repos.append(repo);
    }
    return repos;
}

QList<HfClient::File> HfClient::parseFiles(const QByteArray &json)
{
    QList<File> files;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject())
        return files;

    const QJsonArray siblings = doc.object().value(QStringLiteral("siblings")).toArray();
    for (const QJsonValue &value : siblings) {
        const QJsonObject obj = value.toObject();
        const QString name = obj.value(QStringLiteral("rfilename")).toString();
        if (name.isEmpty())
            continue;
        const qint64 size = qint64(obj.value(QStringLiteral("size")).toDouble());
        File file = classifyFile(name, size);
        if (file.isGguf())
            files.append(file);
    }
    return files;
}

HfClient::File HfClient::classifyFile(const QString &name, qint64 size)
{
    File file;
    file.name = name;
    file.size = size;

    const QRegularExpressionMatch split = splitRe().match(name);
    if (split.hasMatch()) {
        file.split = true;
        file.part = split.captured(2).toInt();
        file.parts = split.captured(3).toInt();
    }

    const QRegularExpressionMatch quant = quantRe().match(name);
    if (quant.hasMatch())
        file.quant = quant.captured(1);
    return file;
}

QString HfClient::modelNameFor(const QString &repo, const QString &fileName)
{
    return canonicalHfModelName(repo, fileName);
}

void HfClient::fetchRepoInfo(const QString &repo)
{
    disposeReply(m_repoInfo, this);

    QUrl url(hubBase() + QStringLiteral("/api/models/") + repo);
    QNetworkReply *reply = get(url);
    m_repoInfo = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, repo]() {
        if (m_repoInfo == reply)
            m_repoInfo = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit repoInfoFinished(repo, QString(), false, reply->errorString());
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        emit repoInfoFinished(repo, obj.value(QStringLiteral("sha")).toString(),
                              true, QString());
    });
}
