// Smoke test for TokenManager: list -> issue -> token -> revoke.
// Run manually: ./token-selftest   (needs an unlocked KDE Wallet)
//
// Works in its own wallet folder, never in "ollama-auth" that the proxy reads
// (T-3): a run that dies halfway cannot leave a valid token for the proxy.
// The folder is removed at the end, whatever the outcome.
#include "tokenmanager.h"

#include <QCoreApplication>
#include <QDebug>

namespace {

int run(TokenManager &manager)
{
    QString error;
    const auto before = manager.entries(&error);
    if (!error.isEmpty()) {
        qCritical() << "entries failed:" << error;
        return 1;
    }
    qInfo() << "devices before:" << before.size() << "in folder" << manager.folder();

    QString token;
    if (!manager.issue(QStringLiteral("_selftest"), &token, &error)) {
        qCritical() << "issue failed:" << error;
        return 1;
    }
    qInfo() << "issued token length" << token.size();

    QString readBack;
    if (!manager.token(QStringLiteral("_selftest"), &readBack, &error)) {
        qCritical() << "read failed:" << error;
        return 1;
    }
    qInfo() << "read back matches:" << (readBack == token);
    if (readBack != token)
        return 1;

    if (!manager.revoke(QStringLiteral("_selftest"), &error)) {
        qCritical() << "revoke failed:" << error;
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TokenManager manager(QStringLiteral("ollama-auth-selftest"), nullptr);
    if (manager.folder() == TokenManager::walletFolder()) {
        qCritical() << "refusing to run in the proxy's own wallet folder";
        return 1;
    }
    const int result = run(manager);
    QString error;
    if (!manager.removeFolder(&error))
        qWarning() << "cleanup failed:" << error;
    qInfo() << (result == 0 ? "done" : "FAILED");
    return result;
}
