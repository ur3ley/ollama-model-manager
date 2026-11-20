#ifndef TOKENMANAGER_H
#define TOKENMANAGER_H

#include <QList>
#include <QObject>
#include <QString>

namespace KWallet {
class Wallet;
}

/**
 * Manages Ollama API tokens.
 *
 * Tokens live in KDE Wallet only (wallet "kdewallet", folder "ollama-auth"):
 *   key "nout"           -> token value
 *   key "created:nout"   -> ISO-8601 creation timestamp
 *
 * The built-in proxy reads the wallet directly and keeps only SHA-256 hashes
 * in memory; there is no snippet file and no external reload step.
 */
class TokenManager : public QObject
{
    Q_OBJECT
public:
    struct Entry {
        QString device;
        QString token;
        QString created;
    };

    explicit TokenManager(QObject *parent = nullptr);
    ~TokenManager() override;

    static QString walletName();
    static QString walletFolder();
    static QString endpoint();
    static QString generateToken();

    bool isWalletAvailable() const;

    /**
     * Открыт ли кошелёк прямо сейчас. Проверка пассивная: openWallet() не
     * вызывается, пароль не запрашивается.
     */
    bool walletUnlocked() const;

    /** Как entries(), но при запертом кошельке возвращает пустой список и ошибку. */
    QList<Entry> entriesIfUnlocked(QString *error = nullptr);

    QList<Entry> entries(QString *error = nullptr);

    bool issue(const QString &device, QString *token, QString *error = nullptr);
    bool revoke(const QString &device, QString *error = nullptr);
    bool token(const QString &device, QString *value, QString *error = nullptr);

signals:
    /** Токены изменились (выпуск/отзыв): прокси должен перечитать карту. */
    void tokensChanged();

private:
    bool ensureWallet(QString *error);
    KWallet::Wallet *m_wallet = nullptr;
};

#endif // TOKENMANAGER_H
