// Табличная проверка парсера заголовков на классах входов из аудита (§2).
// Не тест продукта: печатает, как парсер и переписывание заголовков ведут
// себя на пограничных/враждебных запросах. Используется как доказательство
// в tasks/2026-11-20-security-audit.md.
//
// Сборка:
//   g++ -std=c++17 -O1 -g -fPIC -I src $(pkg-config --cflags Qt6Core) \
//     tests/audit/parser-cases.cpp src/proxyhead.cpp -o /tmp/opencode/audit/parser-cases \
//     $(pkg-config --libs Qt6Core)
#include "proxyhead.h"

#include <QByteArray>
#include <QList>
#include <QPair>

#include <cstdio>

namespace {

bool nameIs(const QByteArray &name, const char *literal)
{
    return name.compare(QLatin1String(literal), Qt::CaseInsensitive) == 0;
}

/** Дословно то же, что делает ProxyConnection::handleHead(). */
QByteArray rebuild(const ProxyHeadRequest &req, const QByteArray &device)
{
    QByteArray out;
    out += req.method + ' ' + req.path + ' ' + req.version + "\r\n";
    for (const auto &header : req.headers) {
        const QByteArray &name = header.first;
        if (nameIs(name, "host") || nameIs(name, "x-ollama-device")
            || nameIs(name, "connection") || nameIs(name, "proxy-connection")
            || nameIs(name, "keep-alive") || nameIs(name, "authorization")
            || nameIs(name, "x-api-key") || nameIs(name, "te") || nameIs(name, "trailer")
            || nameIs(name, "upgrade") || nameIs(name, "proxy-authorization")
            || nameIs(name, "proxy-authenticate")) {
            continue;
        }
        out += name + ": " + header.second + "\r\n";
    }
    out += "Host: 127.0.0.1:11434\r\n";
    out += "X-Ollama-Device: " + device + "\r\n";
    out += "Connection: close\r\n\r\n";
    return out;
}

QByteArray escaped(const QByteArray &in, int limit = 400)
{
    QByteArray out;
    for (int i = 0; i < in.size() && i < limit; ++i) {
        const unsigned char c = static_cast<unsigned char>(in.at(i));
        if (c == '\r')
            out += "<CR>";
        else if (c == '\n')
            out += "<LF>";
        else if (c == '\0')
            out += "<NUL>";
        else if (c < 0x20 || c >= 0x7f)
            out += "<" + QByteArray::number(c) + ">";
        else
            out += char(c);
    }
    if (in.size() > limit)
        out += "...";
    return out;
}

int g_findings = 0;

void probe(const char *label, const QByteArray &head, const char *expect,
           qint64 maxBody = 100LL * 1024 * 1024)
{
    const ProxyHeadRequest r = parseProxyHead(head, maxBody);
    const QByteArray token = r.token.isEmpty() ? QByteArrayLiteral("<none>") : r.token;
    std::printf("\n--- %s\n", label);
    std::printf("  status=%d method=%s path=%s token=%s\n", r.errorStatus,
                escaped(r.method, 60).constData(), escaped(r.path, 120).constData(),
                escaped(token, 60).constData());
    std::printf("  ожидаем по ТЗ: %s\n", expect);
    std::printf("  заголовков=%lld\n", static_cast<long long>(r.headers.size()));
    for (const auto &h : r.headers)
        std::printf("    [%s] = [%s]\n", escaped(h.first, 80).constData(),
                    escaped(h.second, 120).constData());
    if (r.errorStatus == 0) {
        const QByteArray out = rebuild(r, QByteArrayLiteral("laptop"));
        std::printf("  уйдёт апстриму:\n");
        const QList<QByteArray> lines = out.split('\n');
        for (const QByteArray &line : lines)
            std::printf("    | %s\n", escaped(line, 200).constData());
    }
    ++g_findings;
}

} // namespace

int main()
{
    std::printf("Парсер заголовков: наблюдаемое поведение на пограничных входах\n");

    probe("Content-Length: -1", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: -1\r\n\r\n",
          "reject (только цифры >= 0)");
    probe("Content-Length: +1", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: +1\r\n\r\n",
          "reject");
    probe("Content-Length: ' 5 '", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length:  5 \r\n\r\n",
          "reject или trim+число (по ТЗ — только цифры)");
    probe("Content-Length: 1e9", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 1e9\r\n\r\n",
          "reject");
    probe("Content-Length: ''", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: \r\n\r\n",
          "reject");
    probe("Content-Length: 5abc", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 5abc\r\n\r\n",
          "reject");
    probe("Content-Length: 99999999999999999999 (qint64 overflow)",
          "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 99999999999999999999\r\n\r\n",
          "413 reject");
    probe("Content-Length: 200000000 (> 100 МиБ)",
          "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 200000000\r\n\r\n", "413");
    probe("Content-Length дважды (одинаковые)",
          "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n",
          "reject (дубликат)");
    probe("Content-Length дважды (разные)",
          "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\nContent-Length: 7\r\n\r\n",
          "reject (дубликат/конфликт)");
    probe("Transfer-Encoding + Content-Length",
          "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\n"
          "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
          "reject (smuggling)");
    probe("Два Transfer-Encoding",
          "POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n"
          "Transfer-Encoding: identity\r\n\r\n0\r\n\r\n",
          "reject");
    probe("Transfer-Encoding: gzip, chunked (неизвестная комбинация)",
          "POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
          "reject/ограничить");
    probe("Hop-by-hop: TE/Trailer/Upgrade/Proxy-Authorization",
          "GET / HTTP/1.1\r\nHost: t\r\nTE: trailers\r\nTrailer: X\r\n"
          "Upgrade: websocket\r\nProxy-Authorization: Basic Zm9v\r\n"
          "Proxy-Connection: keep-alive\r\nKeep-Alive: 300\r\n\r\n",
          "ни один не пересылается");
    probe("obs-fold (сложение строк)",
          "GET / HTTP/1.1\r\nHost: t\r\nX-Long: a\r\n  b\r\n\r\n", "reject (obs-fold запрещён)");
    probe("NUL в пути и заголовке",
          QByteArray("GET /a\0b HTTP/1.1\r\nHost: t\r\nX-N: \0\r\n\r\n", 40),
          "reject (управляющие символы)");
    probe("CR внутри значения заголовка",
          "GET / HTTP/1.1\r\nHost: t\r\nX-A: v\rInjected: 1\r\n\r\n",
          "reject (control chars)");
    probe("CR внутри пути", "GET /a\rb HTTP/1.1\r\nHost: t\r\n\r\n", "reject");
    probe("Пустое имя заголовка после trim",
          "GET / HTTP/1.1\r\nHost: t\r\n   : value\r\n\r\n", "reject");
    probe("8-битные байты в значении", QByteArray("GET / HTTP/1.1\r\nHost: t\r\nX: \xff\xfe\r\n\r\n", 40),
          "reject/не падать");
    probe("Authorization: Bearer <пусто>",
          "GET / HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer \r\n\r\n", "401 (нет токена)");
    probe("Authorization: Bearer<TAB>token",
          "GET / HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer\tsecret\r\n\r\n",
          "по RFC — допустимо (1*SP), по ТЗ «Bearer <token>»");
    probe("Схема в другом регистре", "GET / HTTP/1.1\r\nHost: t\r\nAuthorization: bEaReR secret\r\n\r\n",
          "принять");
    probe("Пробелы вокруг токена",
          "GET / HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer   secret   \r\n\r\n", "принять");
    probe("Два Authorization (разные токены)",
          "GET / HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer first\r\n"
          "Authorization: Bearer second\r\n\r\n",
          "reject (дубликат) или детерминированно одно");
    probe("Authorization без Bearer + X-Api-Key",
          "GET / HTTP/1.1\r\nHost: t\r\nAuthorization: Basic Zm9v\r\nX-Api-Key: secret\r\n\r\n",
          "принять по X-Api-Key");
    probe("HTTP/1.0 без Host", "GET /api/tags HTTP/1.0\r\n\r\n", "принять (Host добавляем)");
    probe("absolute-form URI", "GET http://example.com/api/tags HTTP/1.1\r\nHost: t\r\n\r\n",
          "принять (URI как есть)");
    probe("Expect: 100-continue",
          "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello",
          "пробросить (или ответить 100 самому)");
    probe("Длинный URI (8000 байт)", "GET /" + QByteArray(8000, 'a') + " HTTP/1.1\r\nHost: t\r\n\r\n",
          "414 Request-URI Too Long");
    probe("Длинный метод (5000 байт)",
          QByteArray(5000, 'G') + " / HTTP/1.1\r\nHost: t\r\n\r\n", "501/400");
    probe("1000 заголовков", "GET / HTTP/1.1\r\n"
                             + [] {
                                   QByteArray h;
                                   for (int i = 0; i < 1000; ++i)
                                       h += "X-H" + QByteArray::number(i) + ": v\r\n";
                                   return h;
                               }()
                             + "\r\n",
          "413/431 при превышении лимита количества");
    probe("Только LF вместо CRLF", "GET / HTTP/1.1\nHost: t\nAuthorization: Bearer secret\n\n",
          "reject (CRLF обязателен)");
    probe("Пустой запрос", "\r\n\r\n", "400");
    probe("Пробел только в первой строке", "   \r\n\r\n", "400");
    probe("Нет пробелов в request-line", "GET/HTTP/1.1\r\nHost: t\r\n\r\n", "400");
    probe("Четвёртый токен в request-line", "GET / HTTP/1.1 EXTRA\r\nHost: t\r\n\r\n",
          "400 (мусор после версии)");

    std::printf("\nВсего проб: %d\n", g_findings);
    return 0;
}
