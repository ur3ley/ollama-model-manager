#include "registryclient.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QHash>
#include <QSet>
#include <QUrlQuery>
#include <QtNetwork/QNetworkRequest>

namespace {

const char *kLibraryBase = "https://ollama.com";

QString stripTags(const QString &html)
{
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    QString text = html;
    text.remove(tagRe);
    text.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&#39;"), QLatin1String("'"));

    // Numeric entities: &#39; and &#x27;.
    static const QRegularExpression decRe(QStringLiteral("&#(\\d+);"));
    static const QRegularExpression hexRe(QStringLiteral("&#[xX]([0-9A-Fa-f]+);"));
    QString decoded;
    int last = 0;
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) != QLatin1Char('&'))
            continue;
        const int semi = text.indexOf(QLatin1Char(';'), i);
        if (semi < 0 || semi - i > 10)
            continue;
        const QString entity = text.mid(i, semi - i + 1);
        bool ok = false;
        const QRegularExpressionMatch dec = decRe.match(entity, 0, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        const QRegularExpressionMatch hex = hexRe.match(entity, 0, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        char32_t code = 0;
        if (dec.hasMatch()) {
            code = char32_t(dec.captured(1).toUInt(&ok));
        } else if (hex.hasMatch()) {
            code = char32_t(hex.captured(1).toUInt(&ok, 16));
        }
        if (ok && code > 0 && code < 0x110000) {
            decoded += text.mid(last, i - last);
            decoded += QString::fromUcs4(&code, 1);
            last = semi + 1;
            i = semi;
        }
    }
    if (last > 0) {
        decoded += text.mid(last);
        text = decoded;
    }
    return text.simplified();
}

QString compactNumber(quint64 value)
{
    if (value >= 1000000000ull)
        return QString::number(value / 1e9, 'f', 1) + QStringLiteral("B");
    if (value >= 1000000ull)
        return QString::number(value / 1e6, 'f', 1) + QStringLiteral("M");
    if (value >= 1000ull)
        return QString::number(value / 1e3, 'f', 1) + QStringLiteral("K");
    return QString::number(value);
}

QStringList capabilitiesIn(const QString &chunk)
{
    static const QStringList known = {
        QStringLiteral("tools"), QStringLiteral("thinking"),
        QStringLiteral("vision"), QStringLiteral("embedding")
    };
    QStringList found;
    for (const QString &cap : known) {
        static const QRegularExpression tagRe(QStringLiteral(">([a-z]+)<"));
        QRegularExpressionMatchIterator it = tagRe.globalMatch(chunk);
        while (it.hasNext()) {
            if (it.next().captured(1) == cap) {
                found.append(cap);
                break;
            }
        }
    }
    return found;
}

} // namespace

RegistryClient::RegistryClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
}

QNetworkReply *RegistryClient::get(const QString &url)
{
    QNetworkRequest request{QUrl(url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) ollama-model-manager"));
    return m_nam->get(request);
}

namespace {

// Abort + release a reply without letting its `finished` handler touch the
// member pointer: `abort()` on an already finished reply emits `finished`
// synchronously, which used to null the member and then crash on `->`.
void disposeReply(QNetworkReply *&member, QObject *owner)
{
    QNetworkReply *reply = member;
    member = nullptr;
    if (!reply)
        return;
    reply->disconnect(owner);
    reply->abort();
    reply->deleteLater();
}

} // namespace

void RegistryClient::search(const QString &query)
{
    disposeReply(m_search, this);

    // Empty query means "browse": the library page lists the popular models.
    QUrl url(QString::fromLatin1(kLibraryBase)
             + (query.trimmed().isEmpty() ? QStringLiteral("/library")
                                          : QStringLiteral("/search")));
    if (!query.trimmed().isEmpty()) {
        QUrlQuery params;
        params.addQueryItem(QStringLiteral("q"), query.trimmed());
        url.setQuery(params);
    }

    QNetworkReply *reply = get(url.toString());
    m_search = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_search == reply)
            m_search = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit searchFinished(false, {}, reply->errorString());
            return;
        }
        emit searchFinished(true, parseSearch(QString::fromUtf8(reply->readAll())), QString());
    });
}

void RegistryClient::listTags(const QString &family)
{
    disposeReply(m_tags, this);

    const QString url = QString::fromLatin1(kLibraryBase) + QStringLiteral("/library/")
            + family + QStringLiteral("/tags");
    QNetworkReply *reply = get(url);
    m_tags = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_tags == reply)
            m_tags = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit tagsFinished(false, {}, reply->errorString());
            return;
        }
        emit tagsFinished(true, parseTags(QString::fromUtf8(reply->readAll())), QString());
    });
}

QList<RegistryClient::Model> RegistryClient::parseSearch(const QString &html)
{
    QList<Model> models;
    QSet<QString> seen;

    static const QRegularExpression linkRe(
        QStringLiteral("href=\"/library/([A-Za-z0-9._-]+)\""));
    QList<QRegularExpressionMatch> matches;
    QRegularExpressionMatchIterator it = linkRe.globalMatch(html);
    while (it.hasNext())
        matches.append(it.next());

    static const QRegularExpression descriptionRe(
        QStringLiteral("<p class=\"(?:mt-1 line-clamp-2|max-w-lg break-words)[^\"]*\">(.*?)</p>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression pullsRe(
        QStringLiteral(">([0-9.]+[KMB])</span>\\s*<span[^>]*>(?:&nbsp;)?Pulls"));
    static const QRegularExpression sizeRe(QStringLiteral(">(\\d+(?:\\.\\d+)?[bm])<"));

    for (int i = 0; i < matches.size(); ++i) {
        const QString name = matches.at(i).captured(1);
        if (name.isEmpty() || seen.contains(name))
            continue;
        seen.insert(name);

        const int start = matches.at(i).capturedEnd();
        const int end = (i + 1 < matches.size()) ? matches.at(i + 1).capturedStart()
                                                 : html.size();
        const QString chunk = html.mid(start, end - start);

        Model model;
        model.name = name;

        const QRegularExpressionMatch description = descriptionRe.match(chunk);
        if (description.hasMatch())
            model.description = stripTags(description.captured(1)).trimmed();

        model.capabilities = capabilitiesIn(chunk);

        // Search cards expose the exact count as a title attribute; library
        // cards only show the compact "121.3M Pulls" pair.
        static const QRegularExpression downloadsTitleRe(
            QStringLiteral("title=\"([0-9,]+) downloads\""));
        const QRegularExpressionMatch downloadsTitle = downloadsTitleRe.match(chunk);
        if (downloadsTitle.hasMatch()) {
            QString digits = downloadsTitle.captured(1);
            digits.remove(QLatin1Char(','));
            model.pulls = compactNumber(digits.toULongLong());
        } else {
            const QRegularExpressionMatch pulls = pullsRe.match(chunk);
            if (pulls.hasMatch())
                model.pulls = pulls.captured(1);
        }

        QRegularExpressionMatchIterator sizes = sizeRe.globalMatch(chunk);
        while (sizes.hasNext())
            model.sizes.append(sizes.next().captured(1));

        models.append(model);
    }
    return models;
}

QList<RegistryClient::Tag> RegistryClient::parseTags(const QString &html)
{
    QList<Tag> tags;
    QHash<QString, int> indexOf;

    static const QRegularExpression linkRe(
        QStringLiteral("href=\"/library/([A-Za-z0-9._-]+:[^\"#]+)\""));
    QList<QRegularExpressionMatch> matches;
    QRegularExpressionMatchIterator it = linkRe.globalMatch(html);
    while (it.hasNext())
        matches.append(it.next());

    static const QRegularExpression sizeRe(
        QStringLiteral("(\\d+(?:\\.\\d+)?\\s*(?:GB|MB))"));
    static const QRegularExpression contextRe(QStringLiteral("(\\d+(?:\\.\\d+)?[KM])"));

    for (int i = 0; i < matches.size(); ++i) {
        const QString name = matches.at(i).captured(1);
        if (name.isEmpty())
            continue;

        const int start = matches.at(i).capturedEnd();
        const int end = (i + 1 < matches.size()) ? matches.at(i + 1).capturedStart()
                                                 : html.size();
        const QString text = stripTags(html.mid(start, end - start));

        Tag tag;
        tag.name = name;

        const QRegularExpressionMatch size = sizeRe.match(text);
        if (size.hasMatch())
            tag.size = size.captured(1).remove(QLatin1Char(' '));

        // The first bare "40K" is the context window; sizes such as "5.2GB"
        // are matched by the size regex and must not be taken for context.
        QRegularExpressionMatchIterator ctx = contextRe.globalMatch(text);
        while (ctx.hasNext()) {
            const QString value = ctx.next().captured(1);
            if (!text.contains(value + QLatin1String("B"))) {
                tag.context = value;
                break;
            }
        }

        if (text.contains(QLatin1String("Vision")))
            tag.input = QStringLiteral("Vision");
        else if (text.contains(QLatin1String("Text")))
            tag.input = QStringLiteral("Text");

        // Each row is rendered twice (mobile + desktop layout); merge the
        // fields instead of dropping the richer copy.
        auto existing = indexOf.constFind(name);
        if (existing != indexOf.constEnd()) {
            Tag &merged = tags[existing.value()];
            if (merged.size.isEmpty()) merged.size = tag.size;
            if (merged.context.isEmpty()) merged.context = tag.context;
            if (merged.input.isEmpty()) merged.input = tag.input;
            continue;
        }
        indexOf.insert(name, tags.size());
        tags.append(tag);
    }
    return tags;
}

void RegistryClient::checkManifest(const QString &ref)
{
    disposeReply(m_manifest, this);

    QString family = ref;
    QString tag = QStringLiteral("latest");
    const int colon = ref.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        family = ref.left(colon);
        tag = ref.mid(colon + 1);
    }

    const QString url = QStringLiteral("https://registry.ollama.ai/v2/library/")
            + family + QStringLiteral("/manifests/") + tag;
    QNetworkReply *reply = get(url);
    m_manifest = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, ref]() {
        if (m_manifest == reply)
            m_manifest = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit manifestChecked(ref, QString(), false, reply->errorString());
            return;
        }
        // The local model id printed by `ollama list` is the manifest digest.
        const QByteArray body = reply->readAll();
        const QString digest = QString::fromLatin1(
            QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());
        emit manifestChecked(ref, digest, true, QString());
    });
}
