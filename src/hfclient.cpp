#include "hfclient.h"

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
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ollama-model-manager"));
    return m_nam->get(request);
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

    const QString fileName = QFileInfo(file).fileName();
    const QString finalPath = QDir(destDir).filePath(fileName);
    const QString partPath = finalPath + QStringLiteral(".part");

    auto *out = new QFile(partPath, this);
    if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString err = out->errorString();
        out->deleteLater();
        emit downloadFinished(false, QString(), err);
        return;
    }

    const QUrl url(QString::fromLatin1(kHubBase) + QLatin1Char('/') + repo
                   + QStringLiteral("/resolve/main/") + fileName);
    QNetworkReply *reply = get(url);
    m_download = reply;

    connect(reply, &QNetworkReply::readyRead, this, [this, reply, out]() {
        out->write(reply->readAll());
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
        emit downloadProgress(received, total);
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, out, finalPath, partPath]() {
        if (m_download == reply)
            m_download = nullptr;
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            out->close();
            out->deleteLater();
            QFile::remove(partPath);
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
    QString base = repo.section(QLatin1Char('/'), -1);
    for (const QString &suffix : {QStringLiteral("-GGUF"), QStringLiteral("-gguf")}) {
        if (base.endsWith(suffix))
            base.chop(suffix.size());
    }

    auto sanitize = [](const QString &raw) {
        QString out;
        out.reserve(raw.size());
        for (const QChar ch : raw) {
            const QChar lower = ch.toLower();
            if (lower.isLetterOrNumber() || lower == QLatin1Char('.')
                    || lower == QLatin1Char('_'))
                out.append(lower);
            else if (out.isEmpty() || out.endsWith(QLatin1Char('-')))
                continue;
            else
                out.append(QLatin1Char('-'));
        }
        while (out.endsWith(QLatin1Char('-')))
            out.chop(1);
        return out;
    };

    const File file = classifyFile(fileName, 0);
    QString tag = file.quant;
    if (tag.isEmpty())
        tag = QFileInfo(fileName).completeBaseName();

    return sanitize(base) + QLatin1Char(':') + sanitize(tag);
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
