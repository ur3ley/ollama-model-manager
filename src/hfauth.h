#ifndef HFAUTH_H
#define HFAUTH_H

#include <QString>

/**
 * HuggingFace Hub access token, kept in KDE Wallet.
 *
 * Same wallet as TokenManager (`TokenManager::walletName()`), own folder:
 *   wallet "kdewallet", folder "ollama-model-manager", key "hf-token".
 *
 * Reads are lazy and passive: nothing touches the wallet at application start,
 * and token() only talks to a wallet that is already unlocked — browsing public
 * repositories therefore never stops on a password prompt. Writing
 * (setToken/clear) happens from the "Token…" dialog, i.e. on user action, and
 * may ask to unlock the wallet.
 *
 * When the wallet is unavailable or locked the token is still kept in memory
 * for the rest of the session, so the gated download the user is looking at can
 * finish; setToken() then reports the wallet problem through `error` while the
 * value stays usable.
 */
class HfAuth
{
public:
    /** Wallet name — the one TokenManager uses. */
    static QString walletName();
    /** Folder inside the wallet ("ollama-model-manager"). */
    static QString walletFolder();
    /** Entry name inside the folder ("hf-token"). */
    static QString walletKey();

    /** KWallet is enabled in this session (passive check, no prompt). */
    static bool isAvailable();

    /**
     * Current token. First call tries the wallet if it is already open; an
     * empty result means "no token", not "wallet broken".
     */
    static QString token();
    static bool hasToken();

    /**
     * Save the token: trims spaces/line breaks, keeps it in memory and writes it
     * to the wallet. Returns false (with `error`) when the wallet cannot be
     * written — the token is applied for this session anyway.
     */
    static bool setToken(const QString &token, QString *error = nullptr);

    /** Remove the token from memory and from the wallet. */
    static bool clear(QString *error = nullptr);

    /** Drop the in-memory copy only (wallet untouched). */
    static void forget();

private:
    HfAuth() = delete;
};

#endif // HFAUTH_H
