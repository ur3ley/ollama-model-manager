// libFuzzer-цель для чистого парсера заголовков (src/proxyhead.cpp).
//
// Сборка:
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
//     -fPIC -I src $(pkg-config --cflags Qt6Core) tests/audit/fuzz-head.cpp src/proxyhead.cpp \
//     -o /tmp/opencode/audit/fuzz-head $(pkg-config --libs Qt6Core)
// Запуск:
//   ./fuzz-head corpus -max_total_time=120 -print_final_stats=1
//   OMM_FUZZ_STRICT=1 ./fuzz-head corpus -max_total_time=30   # семантический оракул
//
// Режим OMM_FUZZ_STRICT трапает на управляющих символах, которые парсер
// сохраняет и затем переотправляет апстриму (потенциальный request splitting).
// Это не «крэш памяти», а нарушение инварианта — артефакты смотреть отдельно.
#include "proxyhead.h"

#include <QByteArray>
#include <QList>
#include <QPair>

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace {

bool hasCtl(const QByteArray &value)
{
    for (const char c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u == '\r' || u == '\n' || u == '\0')
            return true;
        if (u < 0x20 && u != '\t')
            return true;
    }
    return false;
}

bool nameIs(const QByteArray &name, const char *literal)
{
    return name.compare(QLatin1String(literal), Qt::CaseInsensitive) == 0;
}

/** Повторяет переписывание заголовков из ProxyConnection::handleHead(). */
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

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    // Ограничение как у реального входа: не больше 32 КиБ заголовков.
    if (size > 32 * 1024)
        return 0;

    const QByteArray input(reinterpret_cast<const char *>(data), static_cast<int>(size));
    const ProxyHeadRequest req = parseProxyHead(input, 100LL * 1024 * 1024);

    // Инварианты кодов: только 0/400/413/414/431/501.
    if (req.errorStatus != 0 && req.errorStatus != 400 && req.errorStatus != 413
        && req.errorStatus != 414 && req.errorStatus != 431 && req.errorStatus != 501) {
        __builtin_trap();
    }

    // Любой отказ полностью очищает запрос: наружу не уходит ни одного поля.
    if (req.errorStatus != 0) {
        if (!req.method.isEmpty() || !req.path.isEmpty() || !req.headers.isEmpty()
            || !req.token.isEmpty() || req.hasContentLength) {
            __builtin_trap();
        }
        return 0;
    }

    // Метод/путь/версия — из первой строки, переводов строк там быть не может.
    if (req.path.contains('\n') || req.method.contains('\n') || req.version.contains('\n'))
        __builtin_trap();

    const QByteArray out = rebuild(req, QByteArrayLiteral("laptop"));
    (void)out;

    if (qEnvironmentVariableIsSet("OMM_FUZZ_STRICT")) {
        // Семантический оракул (не память): управляющие символы в том, что
        // уйдёт апстриму, пустые имена заголовков, лишние границы блока.
        for (const auto &header : req.headers) {
            if (header.first.isEmpty())
                __builtin_trap();
            if (hasCtl(header.first) || hasCtl(header.second))
                __builtin_trap();
        }
        if (hasCtl(req.path) || hasCtl(req.method) || hasCtl(req.version))
            __builtin_trap();
        if (out.count("\r\n\r\n") != 1)
            __builtin_trap();
    }

    return 0;
}
