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
 *   key "admin:nout"     -> "1": the token may use model management endpoints
 *                           when the proxy restricts them ([proxy] admin_api)
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
        bool admin = false;
    };

    explicit TokenManager(QObject *parent = nullptr);
    /**
     * Работа в другой папке кошелька (T-3): token-selftest пишет в
     * «ollama-auth-selftest», а не в боевую папку, которую читает прокси.
     */
    TokenManager(const QString &folder, QObject *parent);
    ~TokenManager() override;

    static QString walletName();
    static QString walletFolder();
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
    /** Флаг admin устройства (S-3); прокси перечитает карту по tokensChanged. */
    bool setAdmin(const QString &device, bool admin, QString *error = nullptr);
    bool token(const QString &device, QString *value, QString *error = nullptr);
    /** Папка кошелька этого экземпляра (по умолчанию walletFolder()). */
    QString folder() const { return m_folder; }
    /** Удалить свою папку целиком (уборка после token-selftest). */
    bool removeFolder(QString *error = nullptr);

signals:
    /** Токены изменились (выпуск/отзыв): прокси должен перечитать карту. */
    void tokensChanged();

private:
    bool ensureWallet(QString *error);
    QString m_folder;
    KWallet::Wallet *m_wallet = nullptr;
    bool m_opening = false;  // walletopen::open() is running (S-10)
};

#endif // TOKENMANAGER_H
