#include "proxyhead.h"

namespace {

bool nameIs(const QByteArray &name, const char *literal)
{
    return name.compare(QLatin1String(literal), Qt::CaseInsensitive) == 0;
}

/** RFC 9110 tchar — единственное, что допустимо в имени заголовка/методе. */
bool isTokenChar(unsigned char c)
{
    if (c >= 'a' && c <= 'z')
        return true;
    if (c >= 'A' && c <= 'Z')
        return true;
    if (c >= '0' && c <= '9')
        return true;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '-': case '.': case '^': case '_':
    case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

bool isToken(const QByteArray &value)
{
    if (value.isEmpty())
        return false;
    for (const char ch : value) {
        if (!isTokenChar(static_cast<unsigned char>(ch)))
            return false;
    }
    return true;
}

/** request-line: только видимый ASCII (0x20..0x7e), без CR/NUL/8-битных. */
bool isVisibleAscii(const QByteArray &value)
{
    for (const char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c > 0x7e)
            return false;
    }
    return true;
}

/** Значение заголовка: видимый ASCII + HTAB; CR/NUL/0x7f+/8-битные — отказ. */
bool isHeaderValueSafe(const QByteArray &value)
{
    for (const char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\t')
            continue;
        if (c < 0x20 || c > 0x7e)
            return false;
    }
    return true;
}

bool isDigits(const QByteArray &value)
{
    if (value.isEmpty())
        return false;
    for (const char ch : value) {
        if (ch < '0' || ch > '9')
            return false;
    }
    return true;
}

constexpr int kMaxHeaders = 100;
constexpr qsizetype kMaxUriBytes = 8192;
constexpr qsizetype kMaxMethodBytes = 32;

} // namespace

ProxyHeadRequest parseProxyHead(const QByteArray &head, qint64 maxBodyBytes)
{
    ProxyHeadRequest request;

    // Любой отказ полностью очищает запрос: наружу не должно утечь ни одного
    // поля, которое апстрим мог бы принять за валидное.
    const auto reject = [&request](int status, const char *detail) {
        request = ProxyHeadRequest();
        request.errorStatus = status;
        request.errorDetail = QByteArray(detail);
        return request;
    };

    QByteArray block = head;
    if (block.endsWith("\r\n\r\n"))
        block.chop(4);
    else if (block.endsWith("\n\n"))
        block.chop(2);

    const QList<QByteArray> lines = block.split('\n');

    // --- request-line -------------------------------------------------------
    QByteArray requestLine = lines.value(0);
    if (requestLine.endsWith('\r'))
        requestLine.chop(1);
    if (!isVisibleAscii(requestLine))
        return reject(400, "control byte in request-line");

    const QList<QByteArray> parts = requestLine.split(' ');
    if (parts.size() != 3 || parts.at(0).isEmpty() || parts.at(1).isEmpty()
        || parts.at(2).isEmpty())
        return reject(400, "malformed request-line");

    request.method = parts.at(0);
    request.path = parts.at(1);
    request.version = parts.at(2);

    if (!isToken(request.method))
        return reject(400, "method is not a token");
    if (request.method.size() > kMaxMethodBytes)
        return reject(501, "method too long");
    if (request.path.size() > kMaxUriBytes)
        return reject(414, "uri too long");
    if (request.version != QByteArrayLiteral("HTTP/1.1")
        && request.version != QByteArrayLiteral("HTTP/1.0"))
        return reject(400, "unsupported http version");

    // --- header lines -------------------------------------------------------
    bool sawContentLength = false;
    bool sawTransferEncoding = false;

    request.headers.reserve(lines.size());
    for (qsizetype i = 1; i < lines.size(); ++i) {
        QByteArray line = lines.at(i);
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.isEmpty())
            continue; // терминатор блока (в т.ч. LF-only вариант)

        // obs-fold: строка, начинающаяся с SP/HTAB, — устаревшее продолжение
        // заголовка. Прокси его не поддерживает: 400, чтобы не расходиться
        // с представлением апстрима.
        if (line.startsWith(' ') || line.startsWith('\t'))
            return reject(400, "obs-fold");
        // Внутри строки CR/NUL/8-битные недопустимы (CR здесь — уже не CRLF).
        if (!isVisibleAscii(line))
            return reject(400, "control byte in header line");

        const qsizetype colon = line.indexOf(':');
        if (colon <= 0)
            return reject(400, "empty header name");

        const QByteArray name = line.left(colon);
        if (!isToken(name))
            return reject(400, "invalid header name");
        const QByteArray value = line.mid(colon + 1).trimmed();
        if (!isHeaderValueSafe(value))
            return reject(400, "invalid header value");

        if (request.headers.size() >= kMaxHeaders)
            return reject(431, "too many headers");
        request.headers.append({name, value});

        if (nameIs(name, "authorization")) {
            if (value.size() > 7 && value.left(7).compare("bearer ", Qt::CaseInsensitive) == 0)
                request.token = value.mid(7).trimmed(); // Bearer приоритетнее X-Api-Key
        } else if (nameIs(name, "x-api-key")) {
            if (request.token.isEmpty())
                request.token = value;
        } else if (nameIs(name, "connection")) {
            // RFC 9110 §7.6.1: заголовки, перечисленные здесь, — hop-by-hop.
            // Собираем их имена (lowercase), чтобы вырезать при релее.
            const QList<QByteArray> options = value.split(',');
            for (const QByteArray &raw : options) {
                const QByteArray option = raw.trimmed().toLower();
                if (option.isEmpty())
                    continue;
                if (!isToken(option))
                    return reject(400, "invalid Connection option");
                request.connectionOptions.append(option);
            }
        } else if (nameIs(name, "transfer-encoding")) {
            sawTransferEncoding = true;
            // RFC 9112 §6.1: chunked — последний coding в списке. Проверяем
            // токены точно (регистр не важен), а не вхождение подстроки:
            // "notchunked" или "chunked, gzip" не должны включать chunked.
            const QList<QByteArray> codings = value.split(',');
            QList<QByteArray> tokens;
            tokens.reserve(codings.size());
            for (const QByteArray &raw : codings) {
                const QByteArray token = raw.trimmed().toLower();
                if (token.isEmpty() || !isToken(token))
                    return reject(400, "invalid Transfer-Encoding");
                tokens.append(token);
            }
            if (tokens.isEmpty() || tokens.constLast() != "chunked")
                return reject(400, "unsupported Transfer-Encoding");
            int chunkedCount = 0;
            for (const QByteArray &token : tokens) {
                if (token == "chunked")
                    ++chunkedCount;
            }
            if (chunkedCount != 1)
                return reject(400, "invalid Transfer-Encoding");
            request.chunked = true;
        } else if (nameIs(name, "content-length")) {
            if (sawContentLength)
                return reject(400, "duplicate Content-Length");
            sawContentLength = true;
            // Строго `^\d{1,19}$`: без знака, пробелов, экспоненты и мусора.
            if (!isDigits(value) || value.size() > 19)
                return reject(400, "invalid Content-Length");
            bool ok = false;
            const qint64 length = value.toLongLong(&ok);
            if (!ok)
                return reject(400, "invalid Content-Length");
            request.hasContentLength = true;
            request.contentLength = length;
            if (length > maxBodyBytes)
                return reject(413, "body too large");
        }
    }

    if (sawTransferEncoding && sawContentLength)
        return reject(400, "both Transfer-Encoding and Content-Length");

    return request;
}
