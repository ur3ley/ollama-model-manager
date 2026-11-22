// Offline test of the OllamaCli job model and InstallService (A-1, A-2, T-1).
//
// Runs against tests/fake-ollama.sh instead of a real `ollama`: the scenarios
// that used to break the app — two pulls at once, a create racing another
// create, a crashing process, a half-done rename, deleting while the editor
// deletes — are replayed in a second, with no daemon and no network.
//   ./ollamajobs-selftest
#include "apppaths.h"
#include "installservice.h"
#include "ollamacli.h"
#include "provenance.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

class OllamaJobsTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void pullsQueueInsteadOfKilling();
    void cancelQueuedAndRunningPull();
    void failedPullReportsTheError();
    void crashedCreateReportsOnce();
    void overlappingCreatesKeepTheirResults();
    void invalidNamesNeverReachTheCli();
    void missingBinaryFailsEveryJob();
    void showParsesVerbatimSystemAndRepeatedParameters();
    void supersededListIsSilent();
    void blobUsageCountsAndCaches();
    void renamePartialKeepsProvenance();
    void deleteDropsProvenanceAndSources();
    void hfImportCleansUpOnlyOnSuccess();
    void pullFromRegistryRecordsProvenance();

private:
    /** What a finished job reported, copied in its finished() slot (the job deletes itself later). */
    struct Result {
        bool done = false;
        bool success = false;
        bool partial = false;
        QString output;
        QString blob;
        int blobReferences = 0;
        OllamaModelParams params;
    };
    QStringList log() const;
    static Result wait(OllamaJob *job, int timeoutMs = 10000);

    QTemporaryDir m_dir;
    QString m_fake;
    QString m_log;
};

void OllamaJobsTest::initTestCase()
{
    // Downloads and models.json go to ~/.qttest, never the user's real ones.
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(m_dir.isValid());
    // A copy with the exec bit: the source tree may have lost it.
    m_fake = m_dir.filePath(QStringLiteral("ollama"));
    QVERIFY(QFile::copy(QStringLiteral(OMM_FAKE_OLLAMA), m_fake));
    QVERIFY(QFile::setPermissions(m_fake, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                                  | QFileDevice::ExeOwner));
    m_log = m_dir.filePath(QStringLiteral("calls.log"));
    qputenv("FAKE_OLLAMA_LOG", m_log.toLocal8Bit());
}

void OllamaJobsTest::init()
{
    QFile::remove(m_log);
}

QStringList OllamaJobsTest::log() const
{
    QFile file(m_log);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

OllamaJobsTest::Result OllamaJobsTest::wait(OllamaJob *job, int timeoutMs)
{
    auto result = std::make_shared<Result>();
    QObject::connect(job, &OllamaJob::finished, job, [result, job](bool ok, const QString &output) {
        result->done = true;
        result->success = ok;
        result->partial = job->partial();
        result->output = output;
        result->blob = job->blob();
        result->blobReferences = job->blobReferences();
        result->params = job->params();
    });
    if (!QTest::qWaitFor([&] { return result->done; }, timeoutMs))
        qWarning("job %llu did not finish in %d ms", job->id(), timeoutMs);
    return *result;
}

void OllamaJobsTest::pullsQueueInsteadOfKilling()
{
    OllamaCli cli(nullptr, m_fake);
    OllamaJob *slow = cli.pullModelAsync(QStringLiteral("slow"));
    OllamaJob *fast = cli.pullModelAsync(QStringLiteral("fast"));
    QVERIFY(slow->id() != fast->id());
    QSignalSpy slowProgress(slow, &OllamaJob::progress);
    QSignalSpy fastProgress(fast, &OllamaJob::progress);
    QSignalSpy slowDone(slow, &OllamaJob::finished);
    QSignalSpy fastDone(fast, &OllamaJob::finished);

    QTRY_VERIFY(slow->state() == OllamaJob::State::Running);
    QCOMPARE(fast->state(), OllamaJob::State::Queued);
    QVERIFY(cli.isPulling());

    QTRY_COMPARE_WITH_TIMEOUT(fastDone.count(), 1, 10000);
    // Both succeeded, in order; the second never killed the first (B-2).
    QCOMPARE(slowDone.count(), 1);
    QVERIFY(slowDone.first().at(0).toBool());
    QVERIFY(fastDone.first().at(0).toBool());
    QVERIFY(slowProgress.count() >= 10);
    QVERIFY(fastProgress.count() >= 1);
    QCOMPARE(slowProgress.last().at(1).toInt(), 100);
    QVERIFY(!cli.isPulling());
}

void OllamaJobsTest::cancelQueuedAndRunningPull()
{
    OllamaCli cli(nullptr, m_fake);
    OllamaJob *running = cli.pullModelAsync(QStringLiteral("slow"));
    OllamaJob *queued = cli.pullModelAsync(QStringLiteral("fast"));
    QSignalSpy runningDone(running, &OllamaJob::finished);
    QSignalSpy queuedDone(queued, &OllamaJob::finished);
    QTRY_VERIFY(running->state() == OllamaJob::State::Running);

    queued->cancel();
    QCOMPARE(queuedDone.count(), 1);
    QCOMPARE(queuedDone.first().at(1).toString(), QStringLiteral("cancelled"));

    running->cancel();
    QTRY_COMPARE(runningDone.count(), 1);
    QVERIFY(!runningDone.first().at(0).toBool());
    QCOMPARE(runningDone.first().at(1).toString(), QStringLiteral("cancelled"));
    QVERIFY(!cli.isPulling());
    // The cancelled queued pull never started a process.
    QVERIFY(!log().contains(QStringLiteral("pull fast")));
}

void OllamaJobsTest::failedPullReportsTheError()
{
    OllamaCli cli(nullptr, m_fake);
    OllamaJob *job = cli.pullModelAsync(QStringLiteral("fail"));
    QSignalSpy done(job, &OllamaJob::finished);
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(!done.first().at(0).toBool());
    QVERIFY(done.first().at(1).toString().contains(QStringLiteral("file does not exist")));
}

void OllamaJobsTest::crashedCreateReportsOnce()
{
    OllamaCli cli(nullptr, m_fake);
    QSignalSpy broadcast(&cli, &OllamaCli::jobFinished);
    OllamaJob *job = cli.createModel(QStringLiteral("crash"), QStringLiteral("FROM x\n"));
    QSignalSpy done(job, &OllamaJob::finished);
    QTRY_COMPARE(done.count(), 1);
    QTest::qWait(200);
    // errorOccurred(Crashed) + finished(CrashExit) used to give two results (B-4).
    QCOMPARE(done.count(), 1);
    QCOMPARE(broadcast.count(), 1);
    QVERIFY(!done.first().at(0).toBool());
}

void OllamaJobsTest::overlappingCreatesKeepTheirResults()
{
    OllamaCli cli(nullptr, m_fake);
    OllamaJob *slow = cli.createModel(QStringLiteral("slowc"), QStringLiteral("FROM base\n"));
    OllamaJob *quick = cli.createModel(QStringLiteral("quick"), QStringLiteral("FROM base\n"));
    QStringList order;
    connect(slow, &OllamaJob::finished, this, [&order] { order << QStringLiteral("slowc"); });
    connect(quick, &OllamaJob::finished, this, [&order] { order << QStringLiteral("quick"); });
    QTRY_COMPARE(order.size(), 2);
    // Each job reports for itself, whatever finishes first (B-1).
    QCOMPARE(order, QStringList({QStringLiteral("quick"), QStringLiteral("slowc")}));
    const QStringList modes = log().filter(QStringLiteral("modelfile-mode"));
    QCOMPARE(modes.size(), 2);
    QStringList paths;
    for (const QString &line : modes) {
        const QStringList parts = line.split(QLatin1Char(' '));
        QCOMPARE(parts.value(1), QStringLiteral("600"));  // S-1: private
        paths << parts.value(2);
    }
    QVERIFY(paths.at(0) != paths.at(1));  // unique names
    // The temporary Modelfiles go with their jobs.
    QTRY_VERIFY(!QFileInfo::exists(paths.at(0)) && !QFileInfo::exists(paths.at(1)));
}

void OllamaJobsTest::invalidNamesNeverReachTheCli()
{
    OllamaCli cli(nullptr, m_fake);
    QVERIFY(!OllamaCli::isValidModelName(QStringLiteral("-rf")));
    QVERIFY(!OllamaCli::isValidModelName(QStringLiteral("a b")));
    QVERIFY(!OllamaCli::isValidModelName(QString()));
    QVERIFY(OllamaCli::isValidModelName(QStringLiteral("hf.co/owner/repo:Q4_K_M")));
    QVERIFY(OllamaCli::isValidModelName(QStringLiteral("qwen3:8b")));

    OllamaJob *job = cli.deleteModelAsync(QStringLiteral("--help"));
    QSignalSpy done(job, &OllamaJob::finished);
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(!done.first().at(0).toBool());
    QVERIFY(done.first().at(1).toString().contains(QStringLiteral("invalid model name")));
    QVERIFY(log().isEmpty());  // S-6: no process was started
}

void OllamaJobsTest::missingBinaryFailsEveryJob()
{
    OllamaCli cli(nullptr, m_dir.filePath(QStringLiteral("no-such-ollama")));
    OllamaJob *job = cli.showModelAsync(QStringLiteral("a:1"));
    QSignalSpy done(job, &OllamaJob::finished);
    QTRY_COMPARE(done.count(), 1);
    QVERIFY(!done.first().at(0).toBool());

    // B-12: without a binary in PATH isAvailable() is false, not a bare "ollama".
    const QByteArray path = qgetenv("PATH");
    qputenv("PATH", m_dir.filePath(QStringLiteral("empty")).toLocal8Bit());
    OllamaCli none;
    qputenv("PATH", path);
    QVERIFY(!none.isAvailable());
}

void OllamaJobsTest::showParsesVerbatimSystemAndRepeatedParameters()
{
    OllamaCli cli(nullptr, m_fake);
    const Result shown = wait(cli.showModelAsync(QStringLiteral("a:1")));
    QVERIFY(shown.done);
    QVERIFY(shown.success);
    const OllamaModelParams &params = shown.params;
    QCOMPARE(params.architecture, QStringLiteral("qwen3"));
    QCOMPARE(params.contextLength, 40960);
    QCOMPARE(params.systemPrompt, QStringLiteral("Line one\n\n    indented"));
    // B-7: several `stop` lines survive.
    QCOMPARE(params.parameters.values(QStringLiteral("stop")).size(), 2);
    QCOMPARE(cli.modelsDir(), qEnvironmentVariableIsSet("OLLAMA_MODELS")
                                      ? qEnvironmentVariable("OLLAMA_MODELS")
                                      : QStringLiteral("/srv/ollama"));
}

void OllamaJobsTest::supersededListIsSilent()
{
    OllamaCli cli(nullptr, m_fake);
    int listed = 0;
    bool lastOk = false;
    int lastCount = 0;
    connect(&cli, &OllamaCli::modelsListed, this,
            [&](bool ok, const QList<OllamaCli::ModelInfo> &models, const QString &) {
        ++listed;
        lastOk = ok;
        lastCount = int(models.size());
    });
    cli.listModelsAsync();
    QTest::qWait(50);
    cli.listModelsAsync();
    QTRY_COMPARE(listed, 1);
    QTest::qWait(300);
    // The killed first request stays silent (B-3).
    QCOMPARE(listed, 1);
    QVERIFY(lastOk);
    QCOMPARE(lastCount, 3);
}

void OllamaJobsTest::blobUsageCountsAndCaches()
{
    OllamaCli cli(nullptr, m_fake);
    const Result usage = wait(cli.blobUsageAsync(QStringLiteral("a:1")));
    QVERIFY(usage.done);
    QCOMPARE(usage.blob, QStringLiteral("/srv/ollama/blobs/sha256-aaaa"));
    QCOMPARE(usage.blobReferences, 2);  // a:1 and b:1
    const int firstShows = log().filter(QStringLiteral("--modelfile")).size();
    QCOMPARE(firstShows, 4);  // the model itself + 3 listed models

    QFile::remove(m_log);
    const Result again = wait(cli.blobUsageAsync(QStringLiteral("c:1")));
    QVERIFY(again.done);
    QCOMPARE(again.blobReferences, 1);
    // Second dialog: the listed models come from the id cache (P-1).
    QCOMPARE(log().filter(QStringLiteral("--modelfile")).size(), 1);
}

void OllamaJobsTest::renamePartialKeepsProvenance()
{
    OllamaCli cli(nullptr, m_fake);
    Provenance store(m_dir.filePath(QStringLiteral("rename.json")));
    InstallService install(&cli, &store);
    Provenance::Entry entry;
    entry.source = QStringLiteral("hf");
    entry.ref = QStringLiteral("owner/repo");
    store.record(QStringLiteral("fail-rm"), entry);

    const Result renamed = wait(install.renameModel(QStringLiteral("fail-rm"), QStringLiteral("renamed")));
    QVERIFY(renamed.done);
    // cp succeeded, rm failed: partial, and the copy keeps its source (B-5).
    QVERIFY(!renamed.success);
    QVERIFY(renamed.partial);
    QCOMPARE(store.lookup(QStringLiteral("renamed")).ref, QStringLiteral("owner/repo"));

    const Result failed = wait(install.renameModel(QStringLiteral("fail-cp"), QStringLiteral("x")));
    QVERIFY(failed.done);
    QVERIFY(!failed.success);
    QVERIFY(!failed.partial);
}

void OllamaJobsTest::deleteDropsProvenanceAndSources()
{
    OllamaCli cli(nullptr, m_fake);
    Provenance store(m_dir.filePath(QStringLiteral("delete.json")));
    InstallService install(&cli, &store);
    const QDir downloads(apppaths::downloadDir());
    QVERIFY(QDir().mkpath(downloads.path()));
    const QStringList shards = {QStringLiteral("M-Q4-00001-of-00002.gguf"),
                                QStringLiteral("M-Q4-00002-of-00002.gguf")};
    const QString foreign = QStringLiteral("M-Q4-MTP-00001-of-00002.gguf");
    for (const QString &name : shards + QStringList{foreign}) {
        QFile file(downloads.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("gguf");
    }
    Provenance::Entry entry;
    entry.source = QStringLiteral("hf");
    entry.ref = QStringLiteral("owner/M");
    entry.file = shards.first();
    store.record(QStringLiteral("a:1"), entry);
    store.record(QStringLiteral("b:1"), entry);
    QSignalSpy changed(&store, &Provenance::changed);

    // Two deletes at once: each cleans up after its own result only (B-11).
    OllamaJob *ok = install.deleteModel(QStringLiteral("a:1"));
    OllamaJob *bad = install.deleteModel(QStringLiteral("fail-rm"));
    QSignalSpy okDone(ok, &OllamaJob::finished);
    QSignalSpy badDone(bad, &OllamaJob::finished);
    QTRY_VERIFY(okDone.count() == 1 && badDone.count() == 1);
    QVERIFY(!store.contains(QStringLiteral("a:1")));
    QVERIFY(store.contains(QStringLiteral("b:1")));
    QCOMPARE(changed.count(), 1);
    for (const QString &name : shards)
        QVERIFY(!QFileInfo::exists(downloads.filePath(name)));
    // B-13: a neighbouring series with the same prefix is not touched.
    QVERIFY(QFileInfo::exists(downloads.filePath(foreign)));
    QFile::remove(downloads.filePath(foreign));
}

void OllamaJobsTest::hfImportCleansUpOnlyOnSuccess()
{
    OllamaCli cli(nullptr, m_fake);
    Provenance store(m_dir.filePath(QStringLiteral("import.json")));
    InstallService install(&cli, &store);
    const QString source = m_dir.filePath(QStringLiteral("X-Q8_0.gguf"));
    auto writeSource = [&source] {
        QFile file(source);
        if (file.open(QIODevice::WriteOnly))
            file.write(QByteArray(1000, 'g'));
    };

    writeSource();
    InstallService::HfImport request;
    request.name = QStringLiteral("crash");
    request.modelfile = QStringLiteral("FROM ") + source + QLatin1Char('\n');
    request.repo = QStringLiteral("owner/X");
    request.file = QStringLiteral("X-Q8_0.gguf");
    request.sha = QStringLiteral("abc123");
    request.sources = {source};
    const Result failed = wait(install.importHf(request));
    QVERIFY(failed.done);
    QVERIFY(!failed.success);
    QVERIFY(QFileInfo::exists(source));  // a retry must not download again
    QVERIFY(!store.contains(QStringLiteral("crash")));

    request.name = QStringLiteral("x-q8");
    OllamaJob *job = install.importHf(request);
    QVariant freed;
    connect(job, &OllamaJob::finished, this, [&freed, job] {
        freed = job->property(InstallService::kFreedBytes);
    });
    const Result imported = wait(job);
    QVERIFY(imported.success);
    QVERIFY(!QFileInfo::exists(source));
    QCOMPARE(freed.toULongLong(), 1000ULL);
    const Provenance::Entry entry = store.lookup(QStringLiteral("x-q8"));
    QCOMPARE(entry.source, QStringLiteral("hf"));
    QCOMPARE(entry.sha, QStringLiteral("abc123"));
}

void OllamaJobsTest::pullFromRegistryRecordsProvenance()
{
    OllamaCli cli(nullptr, m_fake);
    Provenance store(m_dir.filePath(QStringLiteral("pull.json")));
    InstallService install(&cli, &store);
    OllamaJob *job = install.pullFromRegistry(QStringLiteral("fast"));
    // The caller's slot already sees the record: the service connected first.
    bool recordedBeforeCaller = false;
    connect(job, &OllamaJob::finished, this, [&] {
        recordedBeforeCaller = store.contains(QStringLiteral("fast"));
    });
    QVERIFY(wait(job).success);
    QVERIFY(recordedBeforeCaller);
    QCOMPARE(store.lookup(QStringLiteral("fast")).source, QStringLiteral("ollama"));

    QVERIFY(!wait(install.pullFromRegistry(QStringLiteral("fail"))).success);
    QVERIFY(!store.contains(QStringLiteral("fail")));
}

QTEST_GUILESS_MAIN(OllamaJobsTest)
#include "ollamajobs-selftest.moc"
