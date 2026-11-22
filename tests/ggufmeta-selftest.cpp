// Offline self-test for the GGUF metadata reader.
//
// Builds a minimal GGUF (header + metadata + tensor-info table, no tensor
// data) in a temporary file, reads it back through GgufMeta and checks the
// fields plus the chat-template diagnostics.
//
//   ./ggufmeta-selftest        → prints PASS/FAIL lines, exit code 0/1

#include "ggufmeta.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDataStream>
#include <QTemporaryDir>
#include <QFile>
#include <QTextStream>

namespace {

constexpr quint32 T_UINT32 = 4;
constexpr quint32 T_STRING = 8;
constexpr quint32 T_ARRAY = 9;

int g_failures = 0;

void check(bool condition, const QString &what)
{
    QTextStream out(stdout);
    out << (condition ? "PASS " : "FAIL ") << what << "\n";
    if (!condition)
        ++g_failures;
}

void writeBytes(QDataStream &ds, const QByteArray &bytes)
{
    ds.writeRawData(bytes.constData(), bytes.size());
}

void writeGgufString(QDataStream &ds, const QByteArray &s)
{
    ds << quint64(s.size());
    writeBytes(ds, s);
}

void writeStringKv(QDataStream &ds, const QByteArray &key, const QByteArray &value)
{
    writeGgufString(ds, key);
    ds << T_STRING;
    writeGgufString(ds, value);
}

void writeU32Kv(QDataStream &ds, const QByteArray &key, quint32 value)
{
    writeGgufString(ds, key);
    ds << T_UINT32 << value;
}

void writeStringArrayKv(QDataStream &ds, const QByteArray &key, const QList<QByteArray> &values)
{
    writeGgufString(ds, key);
    ds << T_ARRAY;
    ds << T_STRING << quint64(values.size());
    for (const QByteArray &v : values)
        writeGgufString(ds, v);
}

// An array of arrays (allowed by the spec): must be skipped, not rejected (B-14).
void writeNestedArrayKv(QDataStream &ds, const QByteArray &key)
{
    writeGgufString(ds, key);
    ds << T_ARRAY;
    ds << T_ARRAY << quint64(2);
    for (int i = 0; i < 2; ++i) {
        ds << T_UINT32 << quint64(2);
        ds << quint32(i) << quint32(i + 1);
    }
}

void writeTensorInfo(QDataStream &ds, const QByteArray &name, const QList<quint64> &dims)
{
    writeGgufString(ds, name);
    ds << quint32(dims.size());
    for (quint64 d : dims)
        ds << d;
    ds << quint32(0); // tensor type
    ds << quint64(0); // offset
}

QString writeFixture(const QString &path, const QByteArray &chatTemplate)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();

    QDataStream ds(&file);
    ds.setByteOrder(QDataStream::LittleEndian);

    writeBytes(ds, QByteArrayLiteral("GGUF"));
    ds << quint32(3);
    ds << quint64(2);  // tensors
    ds << quint64(7);  // metadata entries

    writeStringKv(ds, QByteArrayLiteral("general.architecture"), QByteArrayLiteral("testarch"));
    writeStringKv(ds, QByteArrayLiteral("general.name"), QByteArrayLiteral("Test Model"));
    writeU32Kv(ds, QByteArrayLiteral("general.file_type"), 18); // Q6_K
    writeU32Kv(ds, QByteArrayLiteral("testarch.context_length"), 8192);
    writeStringArrayKv(ds, QByteArrayLiteral("tokenizer.ggml.tokens"),
                       {QByteArrayLiteral("<s>"), QByteArrayLiteral("</s>"), QByteArrayLiteral("hi")});
    writeNestedArrayKv(ds, QByteArrayLiteral("test.nested_array"));
    writeStringKv(ds, QByteArrayLiteral("tokenizer.chat_template"), chatTemplate);

    writeTensorInfo(ds, QByteArrayLiteral("blk.0.a"), {2, 3}); // 6
    writeTensorInfo(ds, QByteArrayLiteral("blk.0.b"), {10});   // 10

    return path;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // Inspection mode for real files:
    //   ./ggufmeta-selftest /var/lib/ollama/blobs/sha256-...
    if (argc > 1) {
        const GgufMeta meta = GgufMeta::read(QString::fromLocal8Bit(argv[1]));
        QTextStream out(stdout);
        if (!meta.ok) {
            out << "ERROR " << meta.error << "\n";
            return 1;
        }
        out << "architecture    " << meta.architecture << "\n"
            << "name            " << meta.name << "\n"
            << "quantization    " << meta.quantization << "\n"
            << "context         " << meta.contextLength << "\n"
            << "parameters      " << meta.parameterCount << "\n"
            << "tensors         " << meta.tensorCount << "\n"
            << "chat_template   " << (meta.hasChatTemplate ? "yes" : "no") << "\n"
            << "soft_switch     " << (meta.templateHasSoftSwitch ? "yes" : "no") << "\n";
        return 0;
    }

    QTemporaryDir dir;
    if (!dir.isValid()) {
        QTextStream(stdout) << "FAIL cannot create temp dir\n";
        return 1;
    }

    const QByteArray cleanTemplate =
            "{%- if messages[0].role == 'system' %}{{ messages[0].content }}{%- endif %}"
            "{%- for m in messages %}{{ m.content }}{%- endfor %}";
    const QByteArray softSwitchTemplate =
            "{{- if eq .Role \"user\" }}{{ .Content }}"
            "{{- if and $.IsThinkSet $.Think }} /think{{- end }}{{- end }}";
    const QByteArray strippingTemplate =
            "{%- if '</think>' in content %}{{ content.split('</think>')[-1] }}{%- endif %}"
            "{{- '<think>' }}";

    const QString cleanPath = dir.filePath(QStringLiteral("clean.gguf"));
    writeFixture(cleanPath, cleanTemplate);

    GgufMeta meta = GgufMeta::read(cleanPath);
    check(meta.ok, QStringLiteral("clean fixture parses"));
    check(meta.version == 3, QStringLiteral("version is 3"));
    check(meta.architecture == QLatin1String("testarch"), QStringLiteral("architecture"));
    check(meta.name == QLatin1String("Test Model"), QStringLiteral("name"));
    check(meta.quantization == QLatin1String("Q6_K"), QStringLiteral("quantization from file_type"));
    check(meta.contextLength == 8192, QStringLiteral("context length"));
    check(meta.parameterCount == 16, QStringLiteral("parameter count from tensor table"));
    check(meta.hasChatTemplate, QStringLiteral("chat template present"));
    check(!meta.templateHasSoftSwitch, QStringLiteral("clean template has no soft switch"));
    check(!meta.templateHasThinkTags, QStringLiteral("clean template has no think tags"));

    const QString badPath = dir.filePath(QStringLiteral("softswitch.gguf"));
    writeFixture(badPath, softSwitchTemplate);

    GgufMeta bad = GgufMeta::read(badPath);
    check(bad.ok, QStringLiteral("soft-switch fixture parses"));
    check(bad.templateHasSoftSwitch, QStringLiteral("soft switch detected"));

    // A template that only strips a previous "</think>" block is not a soft
    // switch: Ollama's own non-thinking qwen3 GGUFs carry exactly that.
    const QString stripPath = dir.filePath(QStringLiteral("strip.gguf"));
    writeFixture(stripPath, strippingTemplate);
    GgufMeta strip = GgufMeta::read(stripPath);
    check(strip.ok, QStringLiteral("stripping fixture parses"));
    check(!strip.templateHasSoftSwitch,
          QStringLiteral("</think> stripping is not a soft switch"));
    check(strip.templateHasThinkTags, QStringLiteral("think tags detected"));

    // A non-GGUF file must fail gracefully.
    const QString junkPath = dir.filePath(QStringLiteral("junk.gguf"));
    {
        QFile junk(junkPath);
        if (junk.open(QIODevice::WriteOnly))
            junk.write("not a gguf file at all");
    }
    GgufMeta junk = GgufMeta::read(junkPath);
    check(!junk.ok && !junk.error.isEmpty(), QStringLiteral("junk file rejected with a reason"));

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n" : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
