#ifndef TRAYICON_H
#define TRAYICON_H

#include <QObject>
#include <QString>

class ProxyService;
class QAction;
class QMenu;
class QSystemTrayIcon;

/**
 * Иконка в системном трее: открыть окно, токены, статус прокси, выход.
 *
 * Если трея в сессии нет (или сервис недоступен), объект создаётся, но
 * isAvailable() == false — вызывающая сторона тогда не прячет окно в трей.
 */
class TrayIcon : public QObject
{
    Q_OBJECT
public:
    explicit TrayIcon(ProxyService *service, QObject *parent = nullptr);
    ~TrayIcon() override;

    bool isAvailable() const;

    void showMessage(const QString &title, const QString &text);

signals:
    void openWindowRequested();
    void tokensRequested();
    void restartRequested();
    void quitRequested();

private slots:
    void updateStatus();
    void retranslate();

private:
    ProxyService *m_service;
    QSystemTrayIcon *m_tray = nullptr;
    QMenu *m_menu = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_tokensAction = nullptr;
    QAction *m_statusAction = nullptr;
    QAction *m_restartAction = nullptr;
    QAction *m_quitAction = nullptr;
};

#endif // TRAYICON_H
