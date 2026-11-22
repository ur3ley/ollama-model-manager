#include "ggufmeta.h"

#include <QByteArray>
#include <QDataStream>
#include <QFile>
#include <QRegularExpression>

#include <cstring>
#include <limits>

namespace {

// Sanity limits — a corrupt or hostile file must not make us allocate or loop
// forever.
constexpr quint64 kMaxStringSize = 32ull * 1024ull * 1024ull;
constexpr quint64 kMaxArrayCount = 100ull * 1000ull * 1000ull;
constexpr quint64 kMaxTensors = 1000ull * 1000ull;
constexpr quint64 kMaxDim = 1000ull * 1000ull * 1000ull;

enum ValueType : quint32 {
    T_UINT8 = 0,
    T_INT8 = 1,
    T_UINT16 = 2,
    T_INT16 = 3,
    T_UINT32 = 4,
    T_INT32 = 5,
    T_FLOAT32 = 6,
    T_BOOL = 7,
    T_STRING = 8,
    T_ARRAY = 9,
    T_UINT64 = 10,
    T_INT64 = 11,
    T_FLOAT64 = 12,
};

struct Value {
    bool isString = false;
    QString str;
    bool isUint = false;
    quint64 u = 0;
    bool isFloat = false;
    double f = 0;
};

bool readLength(QDataStream &ds, quint64 &len)
{
    ds >> len;
    return ds.status() == QDataStream::Ok && len <= kMaxStringSize;
}

bool skipString(QDataStream &ds)
{
    quint64 len = 0;
    if (!readLength(ds, len))
        return false;
    return ds.skipRawData(qint64(len)) == qint64(len);
}

bool readString(QDataStream &ds, QString &out)
{
    quint64 len = 0;
    if (!readLength(ds, len))
        return false;
    if (len == 0) {
        out.clear();
        return true;
    }
    QByteArray buf;
    buf.resize(int(len));
    if (ds.readRawData(buf.data(), int(len)) != int(len))
        return false;
    out = QString::fromUtf8(buf);
    return true;
}

// Fixed element size for scalar numeric types, 0 if the type is not scalar.
int scalarSize(quint32 type)
{
    switch (type) {
    case T_UINT8:
    case T_INT8:
    case T_BOOL:
        return 1;
    case T_UINT16:
    case T_INT16:
        return 2;
    case T_UINT32:
    case T_INT32:
    case T_FLOAT32:
        return 4;
    case T_UINT64:
    case T_INT64:
    case T_FLOAT64:
        return 8;
    default:
        return 0;
    }
}

bool skipArray(QDataStream &ds, int depth = 0);

// Consumes one value of `type`. When `out` is non-null and the value is a
// scalar string/integer, the value is stored there.
bool consumeValue(QDataStream &ds, quint32 type, Value *out)
{
    if (type == T_ARRAY)
        return skipArray(ds);

    const int size = scalarSize(type);
    if (size > 0) {
        if (out) {
            quint64 raw = 0;
            if (size == 1) {
                quint8 v = 0;
                ds >> v;
                raw = v;
            } else if (size == 2) {
                quint16 v = 0;
                ds >> v;
                raw = v;
            } else if (size == 4) {
                quint32 v = 0;
                ds >> v;
                raw = v;
            } else {
                quint64 v = 0;
                ds >> v;
                raw = v;
            }
            if (ds.status() != QDataStream::Ok)
                return false;
            out->isUint = (type != T_FLOAT32 && type != T_FLOAT64);
            out->u = raw;
            if (type == T_FLOAT32) {
                float value = 0;
                std::memcpy(&value, &raw, sizeof(value));
                out->f = double(value);
                out->isFloat = true;
            } else if (type == T_FLOAT64) {
                std::memcpy(&out->f, &raw, sizeof(out->f));
                out->isFloat = true;
            }
        } else {
            if (ds.skipRawData(size) != size)
                return false;
        }
        return true;
    }

    if (type == T_STRING) {
        if (!out)
            return skipString(ds);
        return readString(ds, out->str) ? (out->isString = true) : false;
    }

    return false; // unknown value type
}

// The spec allows arrays of arrays; a bounded depth keeps a hostile file from
// recursing without end (B-14).
constexpr int kMaxArrayDepth = 4;

bool skipArray(QDataStream &ds, int depth)
{
    quint32 elemType = 0;
    quint64 count = 0;
    ds >> elemType >> count;
    if (ds.status() != QDataStream::Ok || count > kMaxArrayCount)
        return false;

    const int size = scalarSize(elemType);
    if (size > 0) {
        if (count > quint64(std::numeric_limits<qint64>::max()) / quint64(size))
            return false;
        const qint64 bytes = qint64(count) * size;
        return ds.skipRawData(bytes) == bytes;
    }

    if (elemType == T_STRING) {
        for (quint64 i = 0; i < count; ++i) {
            if (!skipString(ds))
                return false;
        }
        return true;
    }

    if (elemType == T_ARRAY && depth < kMaxArrayDepth) {
        for (quint64 i = 0; i < count; ++i) {
            if (!skipArray(ds, depth + 1))
                return false;
        }
        return true;
    }

    return false;
}

} // namespace

QString GgufMeta::quantizationName(quint32 fileType)
{
    // general.file_type values as defined by llama.cpp.
    switch (fileType) {
    case 0: return QStringLiteral("F32");
    case 1: return QStringLiteral("F16");
    case 2: return QStringLiteral("Q4_0");
    case 3: return QStringLiteral("Q4_1");
    case 7: return QStringLiteral("Q8_0");
    case 8: return QStringLiteral("Q5_0");
    case 9: return QStringLiteral("Q5_1");
    case 10: return QStringLiteral("Q2_K");
    case 11: return QStringLiteral("Q3_K_S");
    case 12: return QStringLiteral("Q3_K_M");
    case 13: return QStringLiteral("Q3_K_L");
    case 14: return QStringLiteral("Q4_K_S");
    case 15: return QStringLiteral("Q4_K_M");
    case 16: return QStringLiteral("Q5_K_S");
    case 17: return QStringLiteral("Q5_K_M");
    case 18: return QStringLiteral("Q6_K");
    case 19: return QStringLiteral("IQ2_XXS");
    case 20: return QStringLiteral("IQ2_XS");
    case 21: return QStringLiteral("IQ3_XXS");
    case 22: return QStringLiteral("IQ1_S");
    case 23: return QStringLiteral("IQ4_NL");
    case 24: return QStringLiteral("IQ3_S");
    case 25: return QStringLiteral("IQ2_S");
    case 26: return QStringLiteral("IQ4_XS");
    case 27: return QStringLiteral("IQ1_M");
    case 28: return QStringLiteral("BF16");
    case 29: return QStringLiteral("Q4_0_4_4");
    case 30: return QStringLiteral("Q4_0_4_8");
    case 31: return QStringLiteral("Q4_0_8_8");
    case 32: return QStringLiteral("TQ1_0");
    case 33: return QStringLiteral("TQ2_0");
    case 34: return QStringLiteral("MXFP4");
    default: return QString();
    }
}

GgufMeta GgufMeta::read(const QString &path)
{
    GgufMeta meta;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        meta.error = file.errorString();
        return meta;
    }

    QDataStream ds(&file);
    ds.setByteOrder(QDataStream::LittleEndian);

    char magic[4] = {};
    if (ds.readRawData(magic, 4) != 4) {
        meta.error = QStringLiteral("truncated header");
        return meta;
    }
    if (std::memcmp(magic, "GGUF", 4) != 0) {
        meta.error = QStringLiteral("not a GGUF file");
        return meta;
    }

    ds >> meta.version;
    if (ds.status() != QDataStream::Ok) {
        meta.error = QStringLiteral("truncated header");
        return meta;
    }
    if (meta.version < 2) {
        meta.error = QStringLiteral("unsupported GGUF version");
        return meta;
    }

    ds >> meta.tensorCount >> meta.kvCount;
    if (ds.status() != QDataStream::Ok) {
        meta.error = QStringLiteral("truncated header");
        return meta;
    }

    quint32 fileType = 0;
    bool haveFileType = false;

    for (quint64 i = 0; i < meta.kvCount; ++i) {
        QString key;
        if (!readString(ds, key)) {
            meta.error = QStringLiteral("malformed metadata");
            return meta;
        }
        quint32 type = 0;
        ds >> type;
        if (ds.status() != QDataStream::Ok) {
            meta.error = QStringLiteral("malformed metadata");
            return meta;
        }

        const bool wantArch = key == QLatin1String("general.architecture");
        const bool wantName = key == QLatin1String("general.name");
        const bool wantType = key == QLatin1String("general.file_type");
        const bool wantCtx = key.endsWith(QLatin1String(".context_length"));
        const bool wantTemplate = key == QLatin1String("tokenizer.chat_template");
        const bool wantTemp = key == QLatin1String("general.sampling.temp");
        const bool wantTopP = key == QLatin1String("general.sampling.top_p");
        const bool wantTopK = key == QLatin1String("general.sampling.top_k");
        const bool wanted = wantArch || wantName || wantType || wantCtx || wantTemplate
                || wantTemp || wantTopP || wantTopK;

        Value v;
        if (!consumeValue(ds, type, wanted ? &v : nullptr)) {
            meta.error = QStringLiteral("malformed metadata");
            return meta;
        }
        if (!wanted)
            continue;

        if (wantArch && v.isString)
            meta.architecture = v.str;
        else if (wantName && v.isString)
            meta.name = v.str;
        else if (wantTemplate && v.isString) {
            meta.hasChatTemplate = true;
            meta.chatTemplate = v.str;
        } else if (wantType && v.isUint) {
            fileType = quint32(v.u);
            haveFileType = true;
        } else if (wantCtx && v.isUint) {
            meta.contextLength = int(qMin<quint64>(v.u, quint64(std::numeric_limits<int>::max())));
        } else if (wantTemp && (v.isFloat || v.isUint)) {
            meta.samplingTemperature = v.isFloat ? v.f : double(v.u);
        } else if (wantTopP && (v.isFloat || v.isUint)) {
            meta.samplingTopP = v.isFloat ? v.f : double(v.u);
        } else if (wantTopK && (v.isFloat || v.isUint)) {
            meta.samplingTopK = v.isFloat ? int(v.f) : int(v.u);
        }
    }

    if (haveFileType)
        meta.quantization = quantizationName(fileType);

    // Tensor info table: name, n_dims, dims[], type, offset. Summing the
    // element counts gives the parameter count Ollama reports as "30.5B".
    // Some models carry very wide tensors (e.g. sparse per-layer embeddings
    // with a 320M-wide axis), hence the generous per-dimension limit; only
    // the product is guarded against overflow.
    if (meta.tensorCount > 0 && meta.tensorCount <= kMaxTensors) {
        quint64 params = 0;
        bool paramsOk = true;
        for (quint64 i = 0; i < meta.tensorCount && paramsOk; ++i) {
            if (!skipString(ds)) {
                paramsOk = false;
                break;
            }
            quint32 nDims = 0;
            ds >> nDims;
            if (ds.status() != QDataStream::Ok || nDims > 8) {
                paramsOk = false;
                break;
            }
            quint64 elems = 1;
            for (quint32 d = 0; d < nDims; ++d) {
                quint64 dim = 0;
                ds >> dim;
                const quint64 sane = qMax<quint64>(dim, 1);
                if (ds.status() != QDataStream::Ok || sane > kMaxDim
                        || elems > std::numeric_limits<quint64>::max() / sane) {
                    paramsOk = false;
                    break;
                }
                elems *= sane;
            }
            quint32 tensorType = 0;
            quint64 offset = 0;
            ds >> tensorType >> offset;
            if (ds.status() != QDataStream::Ok || !paramsOk) {
                paramsOk = false;
                break;
            }
            params += elems;
        }
        if (paramsOk)
            meta.parameterCount = params;
    }

    const QString &t = meta.chatTemplate;
    if (meta.hasChatTemplate) {
        // Ollama's "thinking" Go templates append a literal " /think" or
        // " /no_think" to the last user message; Jinja templates that merely
        // strip reasoning ("if '</think>' in content") must not trip this.
        static const QRegularExpression softSwitchRe(
            QStringLiteral("(?<!<)/(?:no_)?think\\b"));
        meta.templateHasSoftSwitch = t.contains(QLatin1String("IsThinkSet"))
                || softSwitchRe.match(t).hasMatch();
        meta.templateHasThinkTags = t.contains(QLatin1String("<think>"));
    }

    meta.ok = true;
    return meta;
}
