#ifndef TOKENMAP_H
#define TOKENMAP_H

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

/**
 * Допустимое имя устройства: ^[A-Za-z0-9._-]{1,32}$.
 *
 * Единая политика для выпуска токена (TokenManager::issue), загрузки из
 * кошелька и сборки TokenMap: имя уходит в заголовок X-Ollama-Device и в
 * журнал, поэтому CR/LF/управляющие символы недопустимы (M-3).
 */
inline bool isValidDeviceName(const QString &name)
{
    if (name.isEmpty() || name.size() > 32)
        return false;
    for (const QChar ch : name) {
        const ushort c = ch.unicode();
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

/**
 * Карта «SHA-256(token) → device» для проверки токенов в прокси.
 *
 * В объекте открытые токены не хранятся: setTokens() сразу хеширует входные
 * значения. Проверка предъявленного токена — тоже через хеш, поэтому сравнение
 * идёт по QHash, без побайтового сравнения секретов.
 *
 * I-1 (аудит): сравнение сознательно оставлено «хеш + QHash», без
 * constant-time. Утечка по времени относится к 32-байтному SHA-256 и индексу
 * бакета; восстановить по ней 48-символьный токен нельзя (нужен прообраз
 * SHA-256). constant-time-сравнение здесь не добавило бы безопасности.
 *
 * Зависимости от KWallet нет — модуль можно использовать где угодно.
 */
class TokenMap
{
public:
    void setTokens(const QList<QPair<QByteArray, QString>> &tokens);

    /** Device для открытого токена; пустая строка — токен не принят. */
    QString deviceForToken(const QByteArray &token) const;

    int size() const;
    void clear();

    /** SHA-256 (32 байта) — единственное представление токена в памяти. */
    static QByteArray hashToken(const QByteArray &token);

private:
    QHash<QByteArray, QString> m_devices;
};

#endif // TOKENMAP_H
