// Offline self-test for the HuggingFace repository file -> variant grouping.
//
//   ./hfvariants-selftest
//
// No network and no GUI: the input is a hand-written list of repository files
// and the checks pin the grouping the variant picker relies on.

#include "hfclient.h"
#include "hfvariants.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace {

int g_failures = 0;

void check(bool condition, const QString &what)
{
    QTextStream out(stdout);
    out << (condition ? "PASS " : "FAIL ") << what << "\n";
    if (!condition)
        ++g_failures;
}

HfClient::File makeFile(const QString &name, qint64 size)
{
    return HfClient::classifyFile(name, size);
}

QString labelOf(const QList<HfVariant> &variants, int index)
{
    return index >= 0 && index < variants.size() ? variants.at(index).label : QString();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // --- one split quant is one entry, parts summed --------------------------
    {
        QList<HfClient::File> files;
        // Parts arrive out of order on purpose.
        files.append(makeFile(QStringLiteral("Q4_K_M/model-Q4_K_M-00002-of-00003.gguf"), 600));
        files.append(makeFile(QStringLiteral("Q4_K_M/model-Q4_K_M-00001-of-00003.gguf"), 100));
        files.append(makeFile(QStringLiteral("Q4_K_M/model-Q4_K_M-00003-of-00003.gguf"), 300));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 1, QStringLiteral("3 parts collapse into 1 variant"));
        if (variants.size() == 1) {
            const HfVariant &v = variants.first();
            check(v.split, QStringLiteral("split flag set"));
            check(!v.projector, QStringLiteral("split quant is not a projector"));
            check(v.parts == 3, QStringLiteral("parts = 3"));
            check(v.size == 1000, QStringLiteral("size is the sum of the parts"));
            check(v.quant == QStringLiteral("Q4_K_M"), QStringLiteral("quant from the first part"));
            check(v.label == QStringLiteral("Q4_K_M"), QStringLiteral("label is the quant"));
            check(v.files.size() == 3, QStringLiteral("all parts are kept"));
            check(v.sizes.size() == 3, QStringLiteral("a size for every part"));
            check(v.files == QStringList({QStringLiteral("Q4_K_M/model-Q4_K_M-00001-of-00003.gguf"),
                                          QStringLiteral("Q4_K_M/model-Q4_K_M-00002-of-00003.gguf"),
                                          QStringLiteral("Q4_K_M/model-Q4_K_M-00003-of-00003.gguf")}),
                  QStringLiteral("parts ordered 1..3"));
            check(v.sizes == QList<qint64>({100, 600, 300}),
                  QStringLiteral("sizes follow the part order"));
            check(v.enabled, QStringLiteral("split quant is enabled"));
            check(v.id.endsWith(QStringLiteral("-00001-of-00003.gguf")),
                  QStringLiteral("id is the first part"));
        }
    }

    // --- two parts ----------------------------------------------------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("m-IQ2_M-00001-of-00002.gguf"), 10));
        files.append(makeFile(QStringLiteral("m-IQ2_M-00002-of-00002.gguf"), 20));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 1, QStringLiteral("2 parts collapse into 1 variant"));
        if (!variants.isEmpty()) {
            check(variants.first().size == 30, QStringLiteral("2-part size summed"));
            check(variants.first().parts == 2, QStringLiteral("2-part count is 2"));
            check(labelOf(variants, 0) == QStringLiteral("IQ2_M"), QStringLiteral("2-part label"));
        }
    }

    // --- singles, a split quant and a projector together --------------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("Q8_0/model-Q8_0.gguf"), 8000));
        files.append(makeFile(QStringLiteral("BF16/model-BF16-00001-of-00002.gguf"), 3000));
        files.append(makeFile(QStringLiteral("IQ2_XS/model-IQ2_XS.gguf"), 1000));
        files.append(makeFile(QStringLiteral("mmproj/mmproj-model-F16.gguf"), 600));
        files.append(makeFile(QStringLiteral("BF16/model-BF16-00002-of-00002.gguf"), 500));
        files.append(makeFile(QStringLiteral("Q4_K_M/model-Q4_K_M.gguf"), 4000));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 5, QStringLiteral("mixed list groups to 5 variants"));
        if (variants.size() == 5) {
            // Selectable singles first, smallest first.
            check(labelOf(variants, 0) == QStringLiteral("IQ2_XS"),
                  QStringLiteral("first is the smallest single"));
            check(labelOf(variants, 1) == QStringLiteral("Q4_K_M"),
                  QStringLiteral("second is the next single"));
            check(labelOf(variants, 2) == QStringLiteral("Q8_0"),
                  QStringLiteral("third is the largest single"));
            check(variants.at(0).size == 1000 && variants.at(1).size == 4000
                          && variants.at(2).size == 8000,
                  QStringLiteral("singles sorted by size ascending"));
            check(!variants.at(0).split && !variants.at(0).projector,
                  QStringLiteral("single entry is selectable"));

            check(variants.at(3).split && !variants.at(3).projector,
                  QStringLiteral("split entry comes after the singles"));
            check(variants.at(3).size == 3500, QStringLiteral("split size summed in mixed list"));
            check(variants.at(3).parts == 2, QStringLiteral("split parts in mixed list"));
            check(variants.at(3).enabled, QStringLiteral("split entry in mixed list is enabled"));

            check(variants.at(4).projector, QStringLiteral("projector entry is last"));
            check(variants.at(4).split == false, QStringLiteral("single projector is not split"));
            check(!variants.at(4).enabled, QStringLiteral("projector entry stays disabled"));
        }
    }

    // --- duplicates (same label and size) -----------------------------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("model-Q4_K_M.gguf"), 4000));
        files.append(makeFile(QStringLiteral("model-copy-Q4_K_M.gguf"), 4000));
        files.append(makeFile(QStringLiteral("model-Q4_K_M-00001-of-00002.gguf"), 4000));
        files.append(makeFile(QStringLiteral("model-Q4_K_M-00002-of-00002.gguf"), 4000));

        const QList<HfVariant> variants = groupHfFiles(files);
        // Single 4 GB Q4_K_M (two published copies) and a 8 GB split Q4_K_M:
        // same label, different sizes, so neither hides the other.
        check(variants.size() == 2,
              QStringLiteral("duplicate single dropped, single and split kept"));

        int q4Singles = 0;
        int q4Splits = 0;
        for (const HfVariant &v : variants) {
            if (v.label != QStringLiteral("Q4_K_M"))
                continue;
            if (v.split) {
                ++q4Splits;
                check(v.size == 8000, QStringLiteral("split duplicate size summed"));
            } else {
                ++q4Singles;
                check(v.size == 4000, QStringLiteral("single duplicate size kept"));
            }
        }
        check(q4Singles == 1, QStringLiteral("published single duplicate dropped"));
        check(q4Splits == 1, QStringLiteral("split duplicate of one quant stays one entry"));
    }

    // --- exact duplicate drop ----------------------------------------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("a-Q4_K_M.gguf"), 4000));
        files.append(makeFile(QStringLiteral("b-Q4_K_M.gguf"), 4000));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 1,
              QStringLiteral("two published copies of one quant -> one variant"));
    }

    // --- split without a recognized quant -----------------------------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("model-00001-of-00002.gguf"), 10));
        files.append(makeFile(QStringLiteral("model-00002-of-00002.gguf"), 20));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 1, QStringLiteral("quantless split collapses"));
        if (!variants.isEmpty()) {
            check(variants.first().label == QStringLiteral("model"),
                  QStringLiteral("label falls back to the name without the part suffix"));
            check(variants.first().size == 30, QStringLiteral("quantless split size summed"));
        }
    }

    // --- single file without a recognized quant ----------------------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("sub/model.gguf"), 123));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 1, QStringLiteral("quantless single stays one variant"));
        if (!variants.isEmpty()) {
            check(variants.first().label == QStringLiteral("model.gguf"),
                  QStringLiteral("single label drops the directory prefix"));
            check(!variants.first().split && !variants.first().projector,
                  QStringLiteral("quantless single is selectable"));
        }
    }

    // --- trailing quant that HfClient::classifyFile misses -----------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("Qwen3-Coder-Q2_K.gguf"), 100));
        files.append(makeFile(QStringLiteral("Qwen3-Coder-Q6_K.gguf"), 200));
        files.append(makeFile(QStringLiteral("Qwen3-8B.gguf"), 300));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 3, QStringLiteral("three singles stay three variants"));
        if (variants.size() == 3) {
            check(labelOf(variants, 0) == QStringLiteral("Q2_K"),
                  QStringLiteral("bare Q2_K is recovered as the label"));
            check(labelOf(variants, 1) == QStringLiteral("Q6_K"),
                  QStringLiteral("bare Q6_K is recovered as the label"));
            check(labelOf(variants, 2) == QStringLiteral("Qwen3-8B.gguf"),
                  QStringLiteral("a non-quant name keeps the file name"));
        }
    }

    // --- same quant with and without a trailing suffix (MTP) ----------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("m-IQ4_XS-00001-of-00003.gguf"), 100));
        files.append(makeFile(QStringLiteral("m-IQ4_XS-00002-of-00003.gguf"), 100));
        files.append(makeFile(QStringLiteral("m-IQ4_XS-00003-of-00003.gguf"), 10));
        files.append(makeFile(QStringLiteral("m-IQ4_XS-MTP-00001-of-00003.gguf"), 110));
        files.append(makeFile(QStringLiteral("m-IQ4_XS-MTP-00002-of-00003.gguf"), 110));
        files.append(makeFile(QStringLiteral("m-IQ4_XS-MTP-00003-of-00003.gguf"), 10));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 2, QStringLiteral("plain and MTP variants stay separate"));
        if (variants.size() == 2) {
            QSet<QString> labels;
            for (const HfVariant &v : variants)
                labels.insert(v.label);
            check(labels.contains(QStringLiteral("IQ4_XS")),
                  QStringLiteral("plain quant keeps its short label"));
            check(labels.contains(QStringLiteral("IQ4_XS-MTP")),
                  QStringLiteral("MTP suffix is kept in the label"));
        }
    }

    // --- split projector ----------------------------------------------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("mmproj-00001-of-00002.gguf"), 100));
        files.append(makeFile(QStringLiteral("mmproj-00002-of-00002.gguf"), 200));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 1, QStringLiteral("split projector collapses"));
        if (!variants.isEmpty()) {
            check(variants.first().projector, QStringLiteral("split projector keeps its flag"));
            check(variants.first().split, QStringLiteral("split projector keeps the split flag"));
            check(variants.first().parts == 2, QStringLiteral("split projector parts"));
            check(variants.first().size == 300, QStringLiteral("split projector size summed"));
            check(!variants.first().enabled, QStringLiteral("split projector is not installable"));
        }
    }

    // --- projector is never merged with a quant ----------------------------
    {
        QList<HfClient::File> files;
        files.append(makeFile(QStringLiteral("F16/model-F16.gguf"), 1000));
        files.append(makeFile(QStringLiteral("F16/mmproj-model-F16.gguf"), 1000));

        const QList<HfVariant> variants = groupHfFiles(files);
        check(variants.size() == 2,
              QStringLiteral("projector with the same label+size stays separate"));
        if (variants.size() == 2) {
            check(!variants.at(0).projector, QStringLiteral("quant first"));
            check(variants.at(1).projector, QStringLiteral("projector last"));
        }
    }

    // --- Modelfile wildcard prefix -----------------------------------------
    {
        check(splitWildcardPattern(QStringLiteral("model-Q4_K_M-00001-of-00003.gguf"))
                      == QStringLiteral("model-Q4_K_M-\?\?\?\?\?-of-00003.gguf"),
              QStringLiteral("wildcard from the first part"));
        check(splitWildcardPattern(QStringLiteral("Q4_K_M/model-IQ2_XS-00002-of-00002.gguf"))
                      == QStringLiteral("model-IQ2_XS-\?\?\?\?\?-of-00002.gguf"),
              QStringLiteral("wildcard keeps the base name and drops the directory"));
        check(splitWildcardPattern(QStringLiteral("nested/Q8_0/model-Q8_0-00001-of-00012.GGUF"))
                      == QStringLiteral("model-Q8_0-\?\?\?\?\?-of-00012.GGUF"),
              QStringLiteral("wildcard is case-insensitive and keeps the extension case"));
        // B-13: a neighbouring series with the same prefix is not matched.
        {
            const QRegularExpression glob(QRegularExpression::wildcardToRegularExpression(
                    splitWildcardPattern(QStringLiteral("X-Q4_K_M-00001-of-00003.gguf"))));
            check(glob.match(QStringLiteral("X-Q4_K_M-00002-of-00003.gguf")).hasMatch()
                          && !glob.match(QStringLiteral("X-Q4_K_M-MTP-00001-of-00003.gguf")).hasMatch()
                          && !glob.match(QStringLiteral("X-Q4_K_M-00001-of-00004.gguf")).hasMatch(),
                  QStringLiteral("wildcard matches only its own shard series"));
        }
        check(splitWildcardPattern(QStringLiteral("model-Q4_K_M.gguf"))
                      == QStringLiteral("model-Q4_K_M.gguf"),
              QStringLiteral("a single file is not turned into a wildcard"));
        check(splitWildcardPattern(QString())
                      == QString(),
              QStringLiteral("empty name stays empty"));
    }

    // --- empty --------------------------------------------------------------
    check(groupHfFiles({}).isEmpty(), QStringLiteral("empty list -> no variants"));

    QTextStream(stdout) << (g_failures == 0 ? "ALL OK\n"
                                            : "FAILURES: " + QString::number(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
