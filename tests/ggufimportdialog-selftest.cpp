// Offline smoke test for the GGUF import dialog: checks that picking a file
// fills the metadata block and raises the soft-switch warning only when the
// embedded chat template really uses one.
//
// Runs with the offscreen platform plugin, so no display is needed:
//   QT_QPA_PLATFORM=offscreen ./ggufimportdialog-selftest

#include "ggufimportdialog.h"
#include "i18n.h"

#include <QCoreApplication>

#include <QApplication>
#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

namespace {

constexpr quint32 T_UINT32 = 4;
constexpr quint32 T_STRING = 8;

int g_failures = 0;

void check(bool condition, const QString &what)
{
    QTextStream out(stdout);
    out << (condition ? "PASS " : "FAIL ") << what << "\n";
    if (!condition)
        ++g_failures;
}

void writeGgufString(QDataStream &ds, const QByteArray &s)
{
    ds << quint64(s.size());
    ds.writeRawData(s.constData(), s.size());
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

QString writeFixture(const QString &path, const QByteArray &chatTemplate)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();

    QDataStream ds(&file);
    ds.setByteOrder(QDataStream::LittleEndian);

    ds.writeRawData("GGUF", 4);
    ds << quint32(3);
    ds << quint64(1);  // tensors
    ds << quint64(4);  // metadata entries

    writeStringKv(ds, QByteArrayLiteral("general.architecture"), QByteArrayLiteral("testarch"));
    writeStringKv(ds, QByteArrayLiteral("general.name"), QByteArrayLiteral("Dialog Test"));
    writeU32Kv(ds, QByteArrayLiteral("general.file_type"), 15); // Q4_K_M
    writeStringKv(ds, QByteArrayLiteral("tokenizer.chat_template"), chatTemplate);

    writeGgufString(ds, QByteArrayLiteral("blk.0.weight"));
    ds << quint32(2) << quint64(4) << quint64(8);
    ds << quint32(0) << quint64(0);  // type, offset

    return path;
}

} // namespace

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    auto *i18n = I18n::instance();
    i18n->setLanguagesDir(QCoreApplication::applicationDirPath() + "/languages");
    i18n->setLanguage(QStringLiteral("en"));

    // Dev aid: render the dialog with a soft-switch fixture to a PNG.
    //   ./ggufimportdialog-selftest --grab /tmp/dialog.png
    if (argc > 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--grab")) {
        QTemporaryDir tmp;
        const QString fixture = writeFixture(tmp.filePath(QStringLiteral("qwen3-30b-a3b-instruct-2507-Q4_K_M.gguf")),
                 "{{- if and $.IsThinkSet $.Think }} /think{{- end }}");
        GGUFImportDialog dlg;
        dlg.setGgufPath(fixture);
        dlg.resize(560, 620);
        dlg.show();
        QCoreApplication::processEvents();
        const bool saved = dlg.grab().save(QString::fromLocal8Bit(argv[2]));
        QTextStream(stdout) << (saved ? "saved\n" : "save failed\n");
        return saved ? 0 : 1;
    }

    QTemporaryDir dir;
    if (!dir.isValid()) {
        QTextStream(stdout) << "FAIL cannot create temp dir\n";
        return 1;
    }

    const QString cleanPath = dir.filePath(QStringLiteral("clean.gguf"));
    writeFixture(cleanPath,
                 "{%- for m in messages %}{{ m.content }}{%- endfor %}");

    {
        GGUFImportDialog dlg;
        dlg.setGgufPath(cleanPath);
        const QString meta = dlg.metaSummary();
        check(meta.contains(QLatin1String("testarch")), QStringLiteral("summary shows architecture"));
        check(meta.contains(QLatin1String("Q4_K_M")), QStringLiteral("summary shows quantization"));
        check(meta.contains(QLatin1String("32")), QStringLiteral("summary shows parameter count"));
        check(!dlg.templateWarningVisible(), QStringLiteral("no warning for a clean template"));
        check(dlg.modelName().startsWith(QLatin1String("clean")),
              QStringLiteral("model name pre-filled from file name"));
        check(dlg.generatedModelfile().contains(cleanPath),
              QStringLiteral("generated Modelfile references the file"));
    }

    const QString badPath = dir.filePath(QStringLiteral("broken.gguf"));
    writeFixture(badPath,
                 "{{- if and $.IsThinkSet $.Think }} /think{{- end }}");

    {
        GGUFImportDialog dlg;
        dlg.setGgufPath(badPath);
        check(dlg.templateWarningVisible(), QStringLiteral("warning for a soft-switch template"));
        check(!dlg.templateWarning().isEmpty(), QStringLiteral("warning text is filled"));
    }

    {
        GGUFImportDialog dlg;
        dlg.setGgufPath(dir.filePath(QStringLiteral("missing.gguf")));
        check(dlg.metaSummary().contains(QLatin1String("missing.gguf"))
                  || !dlg.metaSummary().isEmpty(),
              QStringLiteral("missing file is reported, not crashing"));
        check(!dlg.templateWarningVisible(), QStringLiteral("no warning when metadata failed"));
    }

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
