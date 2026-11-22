#ifndef WALLETOPEN_H
#define WALLETOPEN_H

#include <QEventLoop>
#include <QObject>
#include <QString>
#include <QTimer>

#include <kwallet.h>

/**
 * Opens a KDE wallet without blocking the event loop (S-10).
 *
 * KWallet::Wallet::Synchronous is a blocking D-Bus call that lasts as long as
 * the password prompt: the window stopped repainting and the timers stalled.
 * Here the wallet is opened Asynchronously and a local event loop waits for
 * walletOpened(); user input is excluded from that loop, so the caller cannot
 * be re-entered by a click, while painting, timers and sockets keep running.
 * The proxy itself lives in its own thread (P-2) and is not affected either way.
 *
 * Returns the open wallet (caller owns it) or nullptr.
 */
namespace walletopen {

inline KWallet::Wallet *open(const QString &name)
{
    KWallet::Wallet *wallet = KWallet::Wallet::openWallet(name, 0, KWallet::Wallet::Asynchronous);
    if (!wallet)
        return nullptr;
    bool opened = false;
    QEventLoop loop;
    QObject::connect(wallet, &KWallet::Wallet::walletOpened, &loop, [&](bool ok) {
        opened = ok;
        loop.quit();
    });
    // The reply arrives through the event loop, never inside openWallet():
    // connecting afterwards cannot miss it. An already open handle is fine too.
    // A daemon that never answers must not hang the caller for good: the
    // password prompt gets five minutes.
    if (!wallet->isOpen()) {
        QTimer::singleShot(5 * 60 * 1000, &loop, &QEventLoop::quit);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    else
        opened = true;
    if (!opened || !wallet->isOpen()) {
        delete wallet;
        return nullptr;
    }
    return wallet;
}

} // namespace walletopen

#endif // WALLETOPEN_H
