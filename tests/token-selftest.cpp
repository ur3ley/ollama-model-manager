// Smoke test for TokenManager: list -> issue -> token -> revoke.
// Run manually: ./token-selftest   (needs an unlocked KDE Wallet)
#include "tokenmanager.h"

#include <QCoreApplication>
#include <QDebug>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    TokenManager manager;
    QString error;

    const auto before = manager.entries(&error);
    if (!error.isEmpty()) {
        qCritical() << "entries failed:" << error;
        return 1;
    }
    qInfo() << "devices before:" << before.size();
    for (const auto &entry : before)
        qInfo() << "  -" << entry.device << entry.created;

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

    if (!manager.revoke(QStringLiteral("_selftest"), &error)) {
        qCritical() << "revoke failed:" << error;
        return 1;
    }
    qInfo() << "done";
    return 0;
}
