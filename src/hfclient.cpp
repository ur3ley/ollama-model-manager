#include "hfclient.h"

#include "quantmatch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSharedPointer>
#include <QtNetwork/QNetworkRequest>
#include <QUrlQuery>

namespace {

const char *kHubBase = "https://huggingface.co";

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
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ollama-model-manager"));
    const QString value = normalizeToken(token);
    if (!value.isEmpty())
        request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + value.toUtf8());
    return request;
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

    QUrl url(QString::fromLatin1(kHubBase) + QStringLiteral("/api/models"));
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

    QUrl url(QString::fromLatin1(kHubBase) + QStringLiteral("/api/models/") + repo);
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

void HfClient::download(const QString &repo, const QString &file, const QString &destDir)
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
    const qint64 alreadyOnDisk = QFileInfo(partPath).exists() ? QFileInfo(partPath).size() : 0;
    auto *out = new QFile(partPath, this);
    QIODevice::OpenMode openMode = QIODevice::WriteOnly;
    openMode |= alreadyOnDisk > 0 ? QIODevice::Append : QIODevice::Truncate;
    if (!out->open(openMode)) {
        const QString err = out->errorString();
        out->deleteLater();
        emit downloadFinished(false, QString(), err);
        return;
    }
    const auto offset = QSharedPointer<qint64>::create(alreadyOnDisk);
    const auto probed = QSharedPointer<bool>::create(false);

    const QUrl url(QString::fromLatin1(kHubBase) + QLatin1Char('/') + repo
                   + QStringLiteral("/resolve/main/") + file);
    QNetworkRequest request = requestFor(url, m_token);
    if (*offset > 0) {
        request.setRawHeader("Range",
                             QByteArrayLiteral("bytes=")
                                     + QByteArray::number(*offset) + QByteArrayLiteral("-"));
    }
    QNetworkReply *reply = m_nam->get(request);
    m_download = reply;

    connect(reply, &QNetworkReply::readyRead, this, [reply, out, offset, probed]() {
        if (!*probed) {
            *probed = true;
            // A server that ignores Range answers 200 with the whole body: drop
            // the partial bytes we were going to append to.
            if (*offset > 0
                && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 206) {
                out->close();
                if (out->open(QIODevice::WriteOnly | QIODevice::Truncate))
                    *offset = 0;
            }
        }
        out->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, offset](qint64 received, qint64 total) {
        const qint64 done = *offset + received;
        emit downloadProgress(done, total > 0 ? *offset + total : -1);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, out, finalPath, partPath]() {
        if (m_download == reply)
            m_download = nullptr;
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            out->close();
            out->deleteLater();
            // Keep the `.part`: the next attempt resumes from its length.
            const bool cancelled = reply->error() == QNetworkReply::OperationCanceledError;
            emit downloadFinished(false, QString(),
                                  cancelled ? QStringLiteral("cancelled")
                                            : networkError(reply));
            return;
        }

        // Anything still buffered in the reply after the last readyRead.
        out->write(reply->readAll());
        out->close();
        out->deleteLater();

        QFile::remove(finalPath);
        if (!QFile::rename(partPath, finalPath)) {
            emit downloadFinished(false, QString(),
                                  QStringLiteral("cannot rename ") + partPath);
            return;
        }
        emit downloadFinished(true, finalPath, QString());
    });
}

void HfClient::cancelDownload()
{
    if (m_download)
        m_download->abort();
}

QList<HfClient::Repo> HfClient::parseSearch(const QByteArray &json)
{
    QList<Repo> repos;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray())
        return repos;

    for (const QJsonValue &value : doc.array()) {
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
        for (const QJsonValue &tag : obj.value(QStringLiteral("tags")).toArray())
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

    QUrl url(QString::fromLatin1(kHubBase) + QStringLiteral("/api/models/") + repo);
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
