#include "trayicon.h"

#include "i18n.h"
#include "proxyservice.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>

namespace {

QIcon trayIconFromTheme()
{
    QIcon icon = QIcon::fromTheme(QStringLiteral("view-list-details"));
    if (icon.isNull())
        icon = QIcon::fromTheme(QStringLiteral("applications-system"));
    if (icon.isNull())
        icon = QApplication::windowIcon();
    if (icon.isNull()) {
        if (QStyle *style = QApplication::style())
            icon = style->standardIcon(QStyle::SP_ComputerIcon);
    }
    return icon;
}

} // namespace

TrayIcon::TrayIcon(ProxyService *service, QObject *parent)
    : QObject(parent)
    , m_service(service)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(trayIconFromTheme());

    m_menu = new QMenu();
    m_openAction = m_menu->addAction(QString());
    m_tokensAction = m_menu->addAction(QString());
    connect(m_openAction, &QAction::triggered, this, &TrayIcon::openWindowRequested);
    connect(m_tokensAction, &QAction::triggered, this, &TrayIcon::tokensRequested);
    m_menu->addSeparator();
    m_statusAction = m_menu->addAction(QString());
    m_statusAction->setEnabled(false);
    m_menu->addSeparator();
    m_restartAction = m_menu->addAction(QString());
    m_quitAction = m_menu->addAction(QString());
    connect(m_restartAction, &QAction::triggered, this, &TrayIcon::restartRequested);
    connect(m_quitAction, &QAction::triggered, this, &TrayIcon::quitRequested);

    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::DoubleClick
                    || reason == QSystemTrayIcon::Trigger)
                    emit openWindowRequested();
            });

    if (m_service)
        connect(m_service, &ProxyService::statusChanged, this, &TrayIcon::updateStatus);
    connect(I18n::instance(), &I18n::languageChanged, this, &TrayIcon::retranslate);

    retranslate();
    m_tray->show();
}

TrayIcon::~TrayIcon()
{
    delete m_menu;   // у QMenu нет родителя-QObject
    m_menu = nullptr;
}

bool TrayIcon::isAvailable() const
{
    return m_tray != nullptr && m_tray->isVisible();
}

void TrayIcon::showMessage(const QString &title, const QString &text)
{
    if (!isAvailable())
        return;
    m_tray->showMessage(title, text, QSystemTrayIcon::Information, 5000);
}

void TrayIcon::retranslate()
{
    if (!m_tray)
        return;
    m_openAction->setText(T("tray.open"));
    m_tokensAction->setText(T("tray.tokens"));
    m_restartAction->setText(T("tray.restart"));
    m_quitAction->setText(T("tray.quit"));
    updateStatus();
}

void TrayIcon::updateStatus()
{
    if (!m_tray)
        return;
    const QString status = m_service ? m_service->statusText() : QString();
    if (m_statusAction)
        m_statusAction->setText(status);
    m_tray->setToolTip(status);
}
