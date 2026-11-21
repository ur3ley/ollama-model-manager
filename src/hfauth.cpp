#include "hfauth.h"

#include "hfclient.h"
#include "tokenmanager.h"

#include <kwallet.h>

namespace {

const char *kFolder = "ollama-model-manager";
const char *kKey = "hf-token";

// Session copy. It is also the fallback when the wallet refuses to cooperate:
// a token typed in the dialog must work right away, even on a machine without
// a wallet daemon.
QString s_token;  // last token seen (wallet or dialog); "" when none

/**
 * Wallet handle, opened at most once per session.
 *
 * `allowPrompt` is what separates the two callers:
 *   false — token() while browsing, may only reuse an already unlocked wallet;
 *   true  — "Token…" dialog, may ask the user to unlock it.
 */
KWallet::Wallet *wallet(bool allowPrompt)
{
    static KWallet::Wallet *cached = nullptr;

    if (cached && !cached->isOpen()) {
        delete cached;
        cached = nullptr;
    }
    if (cached)
        return cached;

    if (!KWallet::Wallet::isEnabled())
        return nullptr;
    if (!allowPrompt && !KWallet::Wallet::isOpen(HfAuth::walletName()))
        return nullptr;

    cached = KWallet::Wallet::openWallet(HfAuth::walletName(), 0,
                                         KWallet::Wallet::Synchronous);
    return cached && cached->isOpen() ? cached : nullptr;
}

void walletError(QString *error, const char *what)
{
    if (error)
        *error = QString::fromLatin1(what).arg(HfAuth::walletName());
}

/** Value from the wallet, empty when there is none (or it cannot be read). */
QString readStoredToken()
{
    KWallet::Wallet *w = wallet(false);
    if (!w)
        return QString();
    const QString folder = HfAuth::walletFolder();
    if (!w->hasFolder(folder) || !w->setFolder(folder))
        return QString();
    QString value;
    if (w->readPassword(HfAuth::walletKey(), value) != 0)
        return QString();
    return HfClient::normalizeToken(value);
}

/** Make sure the folder exists and is current; false + error otherwise. */
bool enterFolder(KWallet::Wallet *w, QString *error)
{
    const QString folder = HfAuth::walletFolder();
    if (!w->hasFolder(folder) && !w->createFolder(folder)) {
        if (error)
            *error = QStringLiteral("cannot create folder '%1' in wallet").arg(folder);
        return false;
    }
    if (!w->setFolder(folder)) {
        if (error)
            *error = QStringLiteral("cannot open folder '%1' in wallet").arg(folder);
        return false;
    }
    return true;
}

} // namespace

QString HfAuth::walletName()
{
    // Single source of truth: the proxy tokens and the HF token live in the
    // same wallet, just in different folders.
    return TokenManager::walletName();
}

QString HfAuth::walletFolder()
{
    return QString::fromLatin1(kFolder);
}

QString HfAuth::walletKey()
{
    return QString::fromLatin1(kKey);
}

bool HfAuth::isAvailable()
{
    return KWallet::Wallet::isEnabled();
}

QString HfAuth::token()
{
    if (!s_token.isEmpty())
        return s_token;
    // A negative answer is deliberately not cached: the wallet can be unlocked
    // later in the session, and the "Token…" dialog must reflect that.
    s_token = readStoredToken();
    return s_token;
}

bool HfAuth::hasToken()
{
    return !token().isEmpty();
}

bool HfAuth::setToken(const QString &token, QString *error)
{
    const QString value = HfClient::normalizeToken(token);
    if (value.isEmpty()) {
        // Refuse blank input instead of silently deleting the stored token.
        if (error)
            *error = QStringLiteral("token is empty");
        return false;
    }

    s_token = value;

    KWallet::Wallet *w = wallet(true);
    if (!w) {
        walletError(error, "cannot open wallet '%1'");
        return false;
    }
    if (!enterFolder(w, error))
        return false;
    if (w->writePassword(walletKey(), value) != 0) {
        if (error)
            *error = QStringLiteral("cannot write token to wallet");
        return false;
    }
    return true;
}

bool HfAuth::clear(QString *error)
{
    const bool hadToken = !s_token.isEmpty();
    s_token.clear();

    KWallet::Wallet *w = wallet(true);
    if (!w) {
        if (error)
            *error = hadToken ? QStringLiteral("cannot open wallet '%1'")
                              : QStringLiteral("wallet '%1' is unavailable");
        return false;
    }
    const QString folder = walletFolder();
    if (!w->hasFolder(folder) || !w->setFolder(folder)) {
        // Nothing stored: the in-memory value is gone, that is a success.
        return true;
    }
    if (!w->hasEntry(walletKey())) {
        // Already gone: the requested state is reached, nothing to report.
        return true;
    }
    if (w->removeEntry(walletKey()) != 0) {
        if (error)
            *error = QStringLiteral("cannot remove token from wallet");
        return false;
    }
    return true;
}

void HfAuth::forget()
{
    s_token.clear();
}
