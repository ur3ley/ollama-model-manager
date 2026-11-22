// Offline test of the first-run migrations and path helpers of 2026-11-22.
//
//  - downloads move from ".../OllamaManager/Ollama Model Manager/models" to
//    ".../ollama-manager/models" (finished files and `.part` alike), a taken
//    name stays in place, the empty legacy tree is removed;
//  - language and the tray hint move into config.ini [ui], once;
//  - a config without [proxy] admin_api means "restricted";
//  - FROM lines quote paths with spaces; models.json paths are rebased.
//   ./migration-selftest
#include "apppaths.h"
#include "installservice.h"
#include "modelfilebuilder.h"
#include "ollamacli.h"
#include "provenance.h"
#include "proxysettings.h"
#include "uisettings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

class MigrationTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void downloadDirHasNoSpaces();
    void downloadsMoveOnce();
    void downloadsKeepTakenNames();
    void uiSettingsMoveIntoConfig();
    void adminApiDefaultsToRestricted();
    void ollamaHostSetsTheUpstream();
    void fromLineQuotesSpaces();
    void provenancePathsAreRebased();
    void sourcePatternsMatchOneSeries();

private:
    static void touch(const QString &path, const QByteArray &data = "x");
    QTemporaryDir m_dir;
};

void MigrationTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QVERIFY(m_dir.isValid());
}

void MigrationTest::touch(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(data);
}

void MigrationTest::downloadDirHasNoSpaces()
{
    QVERIFY(!apppaths::downloadDir().contains(QLatin1Char(' ')));
    QVERIFY(apppaths::downloadDir().endsWith(QStringLiteral("/ollama-manager/models")));
    QVERIFY(apppaths::legacyDownloadDir().endsWith(
            QStringLiteral("/OllamaManager/Ollama Model Manager/models")));
}

void MigrationTest::downloadsMoveOnce()
{
    const QString share = m_dir.filePath(QStringLiteral("share1"));
    const QString legacy = share + QStringLiteral("/OllamaManager/Ollama Model Manager/models");
    const QString target = share + QStringLiteral("/ollama-manager/models");
    touch(legacy + QStringLiteral("/a.gguf"), "done");
    touch(legacy + QStringLiteral("/b.gguf.part"), "half");
    touch(legacy + QStringLiteral("/b.gguf.part.rev"), "abc");

    const apppaths::DownloadMigration result = apppaths::migrateDownloads(legacy, target, share);
    QCOMPARE(result.moved, 3);
    QVERIFY(result.skipped.isEmpty());
    QVERIFY(QFileInfo::exists(target + QStringLiteral("/a.gguf")));
    QVERIFY(QFileInfo::exists(target + QStringLiteral("/b.gguf.part")));
    // The emptied legacy tree is gone up to (not including) the data dir.
    QVERIFY(!QFileInfo::exists(share + QStringLiteral("/OllamaManager")));
    QVERIFY(QFileInfo::exists(share));

    // Second start: nothing left to do.
    const apppaths::DownloadMigration again = apppaths::migrateDownloads(legacy, target, share);
    QCOMPARE(again.moved, 0);
    QVERIFY(again.skipped.isEmpty());
}

void MigrationTest::downloadsKeepTakenNames()
{
    const QString share = m_dir.filePath(QStringLiteral("share2"));
    const QString legacy = share + QStringLiteral("/OllamaManager/Ollama Model Manager/models");
    const QString target = share + QStringLiteral("/ollama-manager/models");
    touch(legacy + QStringLiteral("/a.gguf"), "old");
    touch(legacy + QStringLiteral("/c.gguf"), "moves");
    touch(target + QStringLiteral("/a.gguf"), "new");
    touch(share + QStringLiteral("/OllamaManager/other.conf"));

    const apppaths::DownloadMigration result = apppaths::migrateDownloads(legacy, target, share);
    QCOMPARE(result.moved, 1);
    QCOMPARE(result.skipped, QStringList{QStringLiteral("a.gguf")});
    QFile kept(target + QStringLiteral("/a.gguf"));
    QVERIFY(kept.open(QIODevice::ReadOnly));
    QCOMPARE(kept.readAll(), QByteArray("new"));  // never overwritten
    QVERIFY(QFileInfo::exists(legacy + QStringLiteral("/a.gguf")));
    // A non-empty parent (another file of the old app) is left alone.
    QVERIFY(QFileInfo::exists(share + QStringLiteral("/OllamaManager/other.conf")));
}

void MigrationTest::uiSettingsMoveIntoConfig()
{
    const QString legacy = m_dir.filePath(QStringLiteral("legacy.conf"));
    const QString config = m_dir.filePath(QStringLiteral("ui-config.ini"));
    {
        QSettings old(legacy, QSettings::IniFormat);
        old.setValue(QStringLiteral("language"), QStringLiteral("ru"));
        old.setValue(QStringLiteral("trayCloseHintShown"), true);
    }
    QCOMPARE(uisettings::migrateLegacy(legacy, config), 2);
    {
        QSettings ini(config, QSettings::IniFormat);
        QCOMPARE(ini.value(uisettings::languageKey()).toString(), QStringLiteral("ru"));
        QVERIFY(ini.value(uisettings::trayHintKey()).toBool());
    }
    // Once only: a later choice in the new file wins over the old one.
    {
        QSettings ini(config, QSettings::IniFormat);
        ini.setValue(uisettings::languageKey(), QStringLiteral("en"));
    }
    QCOMPARE(uisettings::migrateLegacy(legacy, config), 0);
    QCOMPARE(QSettings(config, QSettings::IniFormat).value(uisettings::languageKey()).toString(),
             QStringLiteral("en"));
    // The old file is untouched: an older build still reads its own value.
    QCOMPARE(QSettings(legacy, QSettings::IniFormat).value(QStringLiteral("language")).toString(),
             QStringLiteral("ru"));
    QCOMPARE(uisettings::migrateLegacy(m_dir.filePath(QStringLiteral("missing.conf")), config), 0);
}

void MigrationTest::adminApiDefaultsToRestricted()
{
    const QString path = m_dir.filePath(QStringLiteral("proxy.ini"));
    ProxySettings::setConfigPathOverride(path);
    {
        QSettings ini(path, QSettings::IniFormat);
        ini.setValue(QStringLiteral("proxy/enabled"), true);
        ini.setValue(QStringLiteral("proxy/public_url"), QString());
    }
    ProxySettings legacy = ProxySettings::load();
    QVERIFY(legacy.restrictAdminApi);
    QVERIFY(legacy.adminApiImplicit);

    QString error;
    QVERIFY(legacy.save(&error));
    QCOMPARE(QSettings(path, QSettings::IniFormat).value(QStringLiteral("proxy/admin_api")).toString(),
             QStringLiteral("restricted"));
    QVERIFY(!ProxySettings::load().adminApiImplicit);

    QSettings(path, QSettings::IniFormat).setValue(QStringLiteral("proxy/admin_api"), QStringLiteral("open"));
    QVERIFY(!ProxySettings::load().restrictAdminApi);
    QSettings(path, QSettings::IniFormat).setValue(QStringLiteral("proxy/admin_api"), QStringLiteral("bogus"));
    QVERIFY(ProxySettings::load().restrictAdminApi);  // anything but "open" stays closed
    ProxySettings::setConfigPathOverride(QString());
}

void MigrationTest::ollamaHostSetsTheUpstream()
{
    QString host;
    quint16 port = 0;
    QVERIFY(ProxySettings::parseOllamaHost(QStringLiteral("10.0.0.5:11500"), &host, &port));
    QCOMPARE(host, QStringLiteral("10.0.0.5"));
    QCOMPARE(port, quint16(11500));
    QVERIFY(ProxySettings::parseOllamaHost(QStringLiteral("http://box.lan"), &host, &port));
    QCOMPARE(host, QStringLiteral("box.lan"));
    QCOMPARE(port, quint16(11434));
    QVERIFY(ProxySettings::parseOllamaHost(QStringLiteral("0.0.0.0"), &host, &port));
    QCOMPARE(host, QStringLiteral("127.0.0.1"));
    QVERIFY(!ProxySettings::parseOllamaHost(QString(), &host, &port));
}

void MigrationTest::fromLineQuotesSpaces()
{
    QCOMPARE(ModelfileBuilder::fromLine(QStringLiteral("/a b/x.gguf")),
             QStringLiteral("FROM \"/a b/x.gguf\"\n"));
    QCOMPARE(ModelfileBuilder::fromLine(QStringLiteral("/plain/x.gguf")),
             QStringLiteral("FROM /plain/x.gguf\n"));
    QCOMPARE(ModelfileBuilder::fromLine(QStringLiteral("qwen3:8b")), QStringLiteral("FROM qwen3:8b\n"));
    // Ollama has no escape inside quotes: a quote in the path stays bare.
    QCOMPARE(ModelfileBuilder::fromLine(QStringLiteral("/a \"b/x.gguf")),
             QStringLiteral("FROM /a \"b/x.gguf\n"));
    // A quoted path is still recognised as a path, not as a base model name.
    QCOMPARE(OllamaCli::extractFromBase(QStringLiteral("FROM \"/a b/x.gguf\"\n")), QString());
}

void MigrationTest::provenancePathsAreRebased()
{
    Provenance store(m_dir.filePath(QStringLiteral("rebase.json")));
    Provenance::Entry entry;
    entry.source = QStringLiteral("hf");
    entry.ref = QStringLiteral("owner/repo");
    entry.file = QStringLiteral("/old dir/models/x.gguf");
    store.record(QStringLiteral("x"), entry);
    entry.file = QStringLiteral("y.gguf");
    store.record(QStringLiteral("y"), entry);

    QCOMPARE(store.rebasePaths(QStringLiteral("/old dir/models"), QStringLiteral("/new/models")), 1);
    QCOMPARE(store.lookup(QStringLiteral("x")).file, QStringLiteral("/new/models/x.gguf"));
    QCOMPARE(store.lookup(QStringLiteral("y")).file, QStringLiteral("y.gguf"));
    // Persisted.
    Provenance reread(store.path());
    QCOMPARE(reread.lookup(QStringLiteral("x")).file, QStringLiteral("/new/models/x.gguf"));
}

void MigrationTest::sourcePatternsMatchOneSeries()
{
    const QString dir = m_dir.filePath(QStringLiteral("sources"));
    for (const char *name : {"M-00001-of-00002.gguf", "M-00002-of-00002.gguf",
                             "M-MTP-00001-of-00002.gguf", "single.gguf"})
        touch(QDir(dir).filePath(QString::fromLatin1(name)));
    Provenance::Entry entry;
    entry.file = QStringLiteral("sub/M-00001-of-00002.gguf");
    QCOMPARE(InstallService::sourceFiles(entry, dir).size(), 2);
    entry.file = QStringLiteral("single.gguf");
    QCOMPARE(InstallService::sourceFiles(entry, dir).size(), 1);
    entry.file.clear();
    QVERIFY(InstallService::sourceFiles(entry, dir).isEmpty());
}

QTEST_GUILESS_MAIN(MigrationTest)
#include "migration-selftest.moc"
