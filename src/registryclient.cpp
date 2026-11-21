#include "registryclient.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QHash>
#include <QSet>
#include <QUrlQuery>
#include <QtNetwork/QNetworkRequest>

namespace {

const char *kLibraryBase = "https://ollama.com";

// Decodes the HTML entities the library pages actually use, without touching
// whitespace: the readme textarea stores markdown, where newlines matter.
QString decodeEntities(const QString &input)
{
    QString text = input;
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
    return text;
}

QString stripTags(const QString &html)
{
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    QString text = html;
    text.remove(tagRe);
    return decodeEntities(text).simplified();
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

void RegistryClient::fetchModelDetails(const QString &family)
{
    if (family.isEmpty())
        return;
    const auto cached = m_modelCache.constFind(family);
    if (cached != m_modelCache.constEnd()) {
        emit modelDetailsReady(family, cached.value(), true);
        return;
    }

    disposeReply(m_modelDetails, this);

    const QString url = QString::fromLatin1(kLibraryBase) + QStringLiteral("/library/") + family;
    QNetworkReply *reply = get(url);
    m_modelDetails = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, family]() {
        if (m_modelDetails == reply)
            m_modelDetails = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit modelDetailsReady(family, {}, false);
            return;
        }
        const ModelDetails details = parseModelPage(QString::fromUtf8(reply->readAll()));
        m_modelCache.insert(family, details);
        emit modelDetailsReady(family, details, true);
    });
}

void RegistryClient::fetchTagDetails(const QString &ref)
{
    if (ref.isEmpty())
        return;
    const auto cached = m_tagCache.constFind(ref);
    if (cached != m_tagCache.constEnd()) {
        emit tagDetailsReady(ref, cached.value(), true);
        return;
    }

    disposeReply(m_tagDetails, this);

    const QString url = QString::fromLatin1(kLibraryBase) + QStringLiteral("/library/") + ref;
    QNetworkReply *reply = get(url);
    m_tagDetails = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, ref]() {
        if (m_tagDetails == reply)
            m_tagDetails = nullptr;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit tagDetailsReady(ref, {}, false);
            return;
        }
        const TagDetails details = parseTagPage(QString::fromUtf8(reply->readAll()));
        m_tagCache.insert(ref, details);
        emit tagDetailsReady(ref, details, true);
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

RegistryClient::ModelDetails RegistryClient::parseModelPage(const QString &html)
{
    ModelDetails details;
    if (html.isEmpty())
        return details;

    // Everything above the readme block describes the model; the readme itself
    // can contain numbers like "30B" that must not end up in the size list.
    const int readmeAt = html.indexOf(QLatin1String("id=\"display\""));
    const QString head = readmeAt > 0 ? html.left(readmeAt) : html;

    static const QRegularExpression metaRe(
        QStringLiteral("<meta[^>]*property=\"og:description\"[^>]*>"));
    static const QRegularExpression contentRe(QStringLiteral("content=\"([^\"]*)\""));
    const QRegularExpressionMatch meta = metaRe.match(html);
    if (meta.hasMatch()) {
        const QRegularExpressionMatch content = contentRe.match(meta.captured(0));
        if (content.hasMatch())
            details.description = decodeEntities(content.captured(1)).trimmed();
    }

    // `ollama.com` keeps the editable markdown in a hidden textarea; the
    // rendered copy in `div#display` is the fallback when the textarea is
    // empty (older pages).
    static const QRegularExpression editorRe(
        QStringLiteral("<textarea[^>]*id=\"editor\"[^>]*>([\\s\\S]*?)</textarea\\s*>"));
    const QRegularExpressionMatch editor = editorRe.match(html);
    if (editor.hasMatch()) {
        details.readmeMarkdown = decodeEntities(editor.captured(1)).trimmed();
    }

    static const QRegularExpression displayRe(
        QStringLiteral("<div\\s+id=\"display\"[^>]*>([\\s\\S]*?)</div\\s*>"));
    const QRegularExpressionMatch display = displayRe.match(html);
    if (display.hasMatch()) {
        const QString text = stripTags(display.captured(1)).trimmed();
        if (details.readmeMarkdown.isEmpty())
            details.readmeMarkdown = text;
        if (details.description.isEmpty())
            details.description = text;
    }

    static const QRegularExpression updatedRe(
        QStringLiteral("Updated[\\s\\S]{0,80}?(\\d+\\s+\\w+\\s+ago)"));
    const QRegularExpressionMatch updated = updatedRe.match(html);
    if (updated.hasMatch())
        details.updated = updated.captured(1).simplified();

    details.capabilities = capabilitiesIn(head);

    static const QRegularExpression sizeRe(QStringLiteral(">(\\d+(?:\\.\\d+)?[bm])<"));
    QRegularExpressionMatchIterator sizes = sizeRe.globalMatch(head);
    while (sizes.hasNext()) {
        const QString size = sizes.next().captured(1);
        if (!details.sizes.contains(size))
            details.sizes.append(size);
    }
    return details;
}

RegistryClient::TagDetails RegistryClient::parseTagPage(const QString &html)
{
    TagDetails details;
    if (html.isEmpty())
        return details;

    // `<span class="hidden sm:block">arch</span><span class="…">qwen3moe</span>`
    auto spanValue = [&html](const char *key) {
        const QRegularExpression re(QStringLiteral(">%1</span>\\s*<span[^>]*>([^<]+)<")
                                    .arg(QLatin1String(key)));
        const QRegularExpressionMatch match = re.match(html);
        return match.hasMatch() ? decodeEntities(match.captured(1)).trimmed() : QString();
    };
    details.architecture = spanValue("arch");
    details.parameters = spanValue("parameters");
    details.quantization = spanValue("quantization");

    // The license row links the blob and prints the full text next to it; the
    // first line is the license name ("Apache License").
    static const QRegularExpression licenseRe(QStringLiteral(
        "<a href=\"[^\"]*/blobs/[^\"]*\">\\s*license\\s*</a>\\s*</div>\\s*<div[^>]*>([\\s\\S]*?)</div>"));
    const QRegularExpressionMatch license = licenseRe.match(html);
    if (license.hasMatch()) {
        const QStringList lines = decodeEntities(license.captured(1)).split(QLatin1Char('\n'));
        for (const QString &line : lines) {
            const QString text = stripTags(line).trimmed();
            if (!text.isEmpty()) {
                details.license = text;
                break;
            }
        }
    }
    return details;
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
