#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHostAddress>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include <unistd.h>

#include "apppaths.h"
#include "i18n.h"
#include "mainwindow.h"
#include "provenance.h"
#include "proxyservice.h"
#include "proxysettings.h"
#include "theme.h"
#include "tokenmanager.h"
#include "trayicon.h"
#include "uisettings.h"

static QString findLanguagesDir()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    // Приложение может быть запущено через симлинк (~/.local/bin/...) — applicationDirPath()
    // в этом случае указывает на каталог симлинка, а не реального бинарника.
    QString realPath = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    if (realPath.isEmpty())
        realPath = appDir;
    const QString realDir = QFileInfo(realPath).absolutePath();

    const QStringList candidates = {
        realDir + "/languages",
        realDir + "/../languages",
        realDir + "/../share/ollama-model-manager/languages",
        "/usr/share/ollama-model-manager/languages",
        "/usr/local/share/ollama-model-manager/languages",
        appDir + "/languages",
        appDir + "/../languages",
    };
    for (const QString &c : candidates) {
        if (QDir(c).exists())
            return QDir(c).absolutePath();
    }
    return realDir + "/languages";
}

// Логи должны быть видны и при запуске из терминала, и в journald юнита.
// qInfo() при не-TTY уходит в journald, поэтому пишем сами.
static void logLine(const QString &message)
{
    // Свой вывод в stderr: qInfo() при не-TTY уходит в journald, а нам нужен
    // одинаковый лог и в терминале, и в юните. Ошибку вывода игнорируем
    // осознанно — сообщать о ней некуда.
    const QByteArray line = message.toLocal8Bit() + '\n';
    if (std::fwrite(line.constData(), 1, size_t(line.size()), stderr) != size_t(line.size()))
        return;
    if (std::fflush(stderr) != 0)
        return;
}

/** "addr:port" / "[::1]:11434" -> host + port. */
static bool splitEndpoint(const QString &text, QString *host, quint16 *port)
{
    const QString value = text.trimmed();
    const qsizetype colon = value.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0 || colon == value.size() - 1)
        return false;

    QString h = value.left(colon);
    if (h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']')))
        h = h.mid(1, h.size() - 2);

    bool ok = false;
    const uint p = value.mid(colon + 1).toUInt(&ok);
    if (!ok || p == 0 || p > 65535)
        return false;

    *host = h.trimmed();
    *port = quint16(p);
    return !host->isEmpty();
}

static QString instanceServerName()
{
    return QStringLiteral("ollama-model-manager-%1").arg(::getuid());
}

/** true — команду принял уже работающий экземпляр (мы второй). */
static bool notifyRunningInstance(const QString &name, const QString &command)
{
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(500))
        return false;

    socket.write(command.toUtf8() + '\n');
    socket.flush();
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState)
        socket.waitForDisconnected(500);
    return true;
}

int main(int argc, char *argv[])
{
    bool i18nDebug = false;
    if (qEnvironmentVariableIntValue("OMM_I18N_DEBUG", &i18nDebug) == 1 && i18nDebug)
        qputenv("QT_FORCE_STDERR_LOGGING", "1"); // Qt иначе уводит логи в journald

    // Класс приложения выбираем до его создания: без дисплея QApplication не
    // стартует, а демону нужен только event loop и QTcpServer.
    bool noTrayFlag = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-tray") == 0)
            noTrayFlag = true;
    }
    const bool hasDisplay = !qEnvironmentVariableIsEmpty("DISPLAY")
                            || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
    const bool graphical = !noTrayFlag && hasDisplay;

    std::unique_ptr<QCoreApplication> appHolder;
    if (graphical)
        appHolder = std::make_unique<QApplication>(argc, argv);
    else
        appHolder = std::make_unique<QCoreApplication>(argc, argv);
    QCoreApplication &app = *appHolder;

    app.setApplicationName("Ollama Model Manager");
    app.setApplicationVersion("1.1.0");
    app.setOrganizationName("OllamaManager");

    // ~/.config/ollama-manager: адреса, апстрим, история загрузок — только
    // владельцу (S-8), как и tls/ внутри. Старый 0755 тоже ужесточается.
    apppaths::ensurePrivateDir(apppaths::configDir());
    apppaths::restrictFile(apppaths::configFile());

    if (auto *guiApp = qobject_cast<QApplication *>(&app)) {
        // Без окна приложение не должно завершаться: прокси живёт в трее.
        guiApp->setQuitOnLastWindowClosed(false);
    }

    // Оформление (Fusion + палитра + QSS) — до создания окон, чтобы окно
    // никогда не показалось в системном стиле, а затем перекрасилось.
    theme::apply();

    // Язык и флаг подсказки трея жили в QSettings по умолчанию
    // (~/.config/OllamaManager/…conf); теперь — [ui] в config.ini. Копируем
    // один раз, старый файл не трогаем (откат — см. JOURNAL.md).
    const int uiMigrated = uisettings::migrateLegacy();

    I18n::instance()->setLanguagesDir(findLanguagesDir());
    QString saved = uisettings::language();
    if (!I18n::instance()->setLanguage(saved)) {
        if (!I18n::instance()->setLanguage("en"))
            qWarning("i18n: ни '%s', ни 'en' не загрузились, каталог языков: %s",
                     qPrintable(saved), qPrintable(I18n::instance()->languagesDir()));
    }

    if (i18nDebug) {
        qInfo("i18n dir=%s lang=%s sample app.title=\"%s\" menu.file=\"%s\"",
              qPrintable(I18n::instance()->languagesDir()),
              qPrintable(I18n::instance()->currentCode()),
              qPrintable(T("app.title")),
              qPrintable(T("menu.file")));
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Ollama Model Manager"));
    parser.addHelpOption();
    parser.addVersionOption();

    const QCommandLineOption daemonOption(
        QStringLiteral("daemon"),
        QStringLiteral("Run in the background: tray icon if available, no window."));
    const QCommandLineOption noTrayOption(
        QStringLiteral("no-tray"),
        QStringLiteral("Headless: no tray and no window (for the systemd unit)."));
    const QCommandLineOption showOption(
        QStringLiteral("show"),
        QStringLiteral("Show the window of the already running instance and exit."));
    const QCommandLineOption listenOption(
        QStringLiteral("listen"),
        QStringLiteral("Listen on addr:port, overriding the config for this run."),
        QStringLiteral("addr:port"));
    const QCommandLineOption upstreamOption(
        QStringLiteral("upstream"),
        QStringLiteral("Upstream Ollama host:port, overriding the config for this run."),
        QStringLiteral("host:port"));
    const QCommandLineOption configOption(
        QStringLiteral("config"),
        QStringLiteral("Alternative config file "
                       "(default ~/.config/ollama-manager/config.ini)."),
        QStringLiteral("path"));
    const QCommandLineOption tlsOption(
        QStringLiteral("tls"),
        QStringLiteral("TLS mode for this run: off, internal, files, letsencrypt."),
        QStringLiteral("mode"));

    parser.addOptions({daemonOption, noTrayOption, showOption, listenOption, upstreamOption,
                       configOption, tlsOption});
    parser.process(app);

    // Single-instance: один процесс на пользователя, иначе два экземпляра
    // подрались бы за порт и за кошелёк.
    const QString serverName = instanceServerName();
    if (notifyRunningInstance(serverName, QStringLiteral("show")))
        return 0; // окно поднял уже работающий экземпляр

    QLocalServer localServer;
    // Только владелец (этот uid) может подключиться к сокету.
    localServer.setSocketOptions(QLocalServer::UserAccessOption);
    if (!localServer.listen(serverName)) {
        // Сокет мог остаться после падения прошлого экземпляра. Удаляем его
        // только если владелец действительно мёртв: живой экземпляр, не
        // успевший ответить за 500 мс, иначе потерял бы сокет (L-5).
        if (localServer.serverError() == QAbstractSocket::AddressInUseError) {
            QLocalSocket probe;
            probe.connectToServer(serverName);
            if (!probe.waitForConnected(300)) {
                QLocalServer::removeServer(serverName);
                localServer.listen(serverName);
            }
        }
    }
    if (!localServer.isListening()) {
        logLine(QStringLiteral("warning: single-instance socket unavailable: %1")
                    .arg(localServer.errorString()));
    }

    if (uiMigrated > 0) {
        logLine(QStringLiteral("settings: language/tray hint moved to %1 [ui]")
                    .arg(apppaths::configFile()));
    }

    // Загрузки GGUF переехали в путь без пробелов (он попадает в FROM).
    // Только rename: файлы, которые не удалось перенести, остаются на месте.
    // Только в GUI: загрузками пользуется окно, а юнит systemd видит
    // ~/.local/share только для чтения (ProtectHome=read-only).
    if (graphical) {
        const QString genericData =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        const apppaths::DownloadMigration moved = apppaths::migrateDownloads(
            apppaths::legacyDownloadDir(), apppaths::downloadDir(), genericData);
        if (moved.moved > 0) {
            logLine(QStringLiteral("downloads: moved %1 file(s) to %2")
                        .arg(moved.moved).arg(apppaths::downloadDir()));
        }
        // models.json хранит имена файлов репозитория, но абсолютный путь в
        // записи (ручная правка, будущий формат) тоже должен остаться верным.
        const int rebased = Provenance::instance().rebasePaths(apppaths::legacyDownloadDir(),
                                                               apppaths::downloadDir());
        if (rebased > 0)
            logLine(QStringLiteral("downloads: %1 path(s) updated in models.json").arg(rebased));
        if (!moved.skipped.isEmpty()) {
            logLine(QStringLiteral("warning: downloads left in %1 (name taken or rename failed): %2")
                        .arg(apppaths::legacyDownloadDir(),
                             moved.skipped.join(QStringLiteral(", "))));
        }
    }

    if (parser.isSet(configOption))
        ProxySettings::setConfigPathOverride(parser.value(configOption));

    ProxySettings settings = ProxySettings::load();

    if (parser.isSet(listenOption)) {
        QString host;
        quint16 port = 0;
        if (!splitEndpoint(parser.value(listenOption), &host, &port)) {
            logLine(QStringLiteral("error: --listen expects addr:port, got '%1'")
                        .arg(parser.value(listenOption)));
            return 2;
        }
        if (QHostAddress(host).isNull()) {
            logLine(QStringLiteral("error: --listen: invalid address '%1'").arg(host));
            return 2;
        }
        if (host == QLatin1String("0.0.0.0") || host == QLatin1String("::")) {
            settings.mode = ProxySettings::AllInterfaces;
            settings.addresses.clear();
        } else {
            settings.mode = ProxySettings::Interfaces;
            settings.addresses = {QHostAddress(host).toString()};
        }
        settings.port = port;
    }

    if (parser.isSet(upstreamOption)) {
        QString host;
        quint16 port = 0;
        if (!splitEndpoint(parser.value(upstreamOption), &host, &port)) {
            logLine(QStringLiteral("error: --upstream expects host:port, got '%1'")
                        .arg(parser.value(upstreamOption)));
            return 2;
        }
        settings.upstreamHost = std::move(host);
        settings.upstreamPort = port;
    }

    if (parser.isSet(tlsOption)) {
        const QString value = parser.value(tlsOption).trimmed().toLower();
        const ProxySettings::TlsMode mode =
            ProxySettings::tlsModeFromName(value, ProxySettings::TlsMode::Off);
        // Пустая/неизвестная строка не должна молча включать TLS.
        if (value.isEmpty() || (mode == ProxySettings::TlsMode::Off
                                && value != QLatin1String("off"))) {
            logLine(QStringLiteral("error: --tls expects off|internal|files|letsencrypt, got '%1'")
                        .arg(value));
            return 2;
        }
        settings.tls = mode;
    }

    TokenManager tokens;
    ProxyService service(&tokens);
    QObject::connect(&service, &ProxyService::logMessage, &app,
                     [](const QString &message) { logLine(QStringLiteral("[proxy] ") + message); });

    // start() сам решает, запускать ли прокси (enabled) и фиксирует действующие
    // настройки для диалога и трея; ошибку он отправляет в logMessage.
    service.start(settings);

    std::unique_ptr<MainWindow> window;
    std::unique_ptr<TrayIcon> trayHolder;
    TrayIcon *tray = nullptr;

    // Окно создаётся лениво: при --daemon его может не быть вовсе, а --show
    // второго экземпляра должен поднять его в уже работающем процессе.
    auto ensureWindow = [&]() -> MainWindow * {
        if (!window) {
            window = std::make_unique<MainWindow>(&tokens, &service);
            if (tray) {
                window->setHideOnClose(true);
                QObject::connect(window.get(), &MainWindow::hiddenToTray, &app, [&]() {
                    // Подсказка — один раз за установку: дальше пользователь уже знает,
                    // что крестик сворачивает окно в трей, а не закрывает приложение.
                    if (uisettings::trayHintShown())
                        return;
                    uisettings::setTrayHintShown();
                    tray->showMessage(T("tray.hiddenTitle"), T("tray.hiddenText"));
                });
            }
        }
        return window.get();
    };

    auto showWindow = [&]() {
        if (!graphical) {
            logLine(QStringLiteral("no window: running headless"));
            return;
        }
        MainWindow *w = ensureWindow();
        w->show();
        w->setWindowState((w->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        w->raise();
        w->activateWindow();
    };

    if (graphical) {
        auto trayIcon = std::make_unique<TrayIcon>(&service);
        if (trayIcon->isAvailable()) {
            tray = trayIcon.get();
            trayHolder = std::move(trayIcon);

            QObject::connect(tray, &TrayIcon::openWindowRequested, &app,
                             [&]() { showWindow(); });
            QObject::connect(tray, &TrayIcon::tokensRequested, &app, [&]() {
                MainWindow *w = ensureWindow();
                showWindow();
                w->showTokensDialog();
            });
            QObject::connect(tray, &TrayIcon::restartRequested, &app, [&]() {
                QString error;
                if (!service.restart(service.settings(), &error))
                    logLine(QStringLiteral("[proxy] restart failed: ") + error);
            });
            QObject::connect(tray, &TrayIcon::quitRequested, &app,
                             []() { QCoreApplication::quit(); });
        }
    }

    if (localServer.isListening()) {
        QObject::connect(&localServer, &QLocalServer::newConnection, &app, [&]() {
            while (QLocalSocket *socket = localServer.nextPendingConnection()) {
                // Команда — не больше 64 байт, и только show/quit: всё
                // остальное молча закрываем (L-5).
                socket->setReadBufferSize(64);
                QObject::connect(socket, &QLocalSocket::readyRead, socket,
                                 [socket, &app, &showWindow]() {
                                     const QByteArray command = socket->read(64).trimmed();
                                     if (command == QByteArrayLiteral("quit"))
                                         app.quit();
                                     else if (command == QByteArrayLiteral("show"))
                                         showWindow();
                                     socket->disconnectFromServer();
                                 });
                QObject::connect(socket, &QLocalSocket::disconnected, socket,
                                 &QObject::deleteLater);
            }
        });
    }

    if (graphical && !parser.isSet(daemonOption))
        showWindow();

    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&]() { service.stop(); });

    return app.exec();
}
