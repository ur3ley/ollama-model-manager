#ifndef PROXYHEAD_H
#define PROXYHEAD_H

#include <QByteArray>
#include <QList>
#include <QPair>

/**
 * Разбор заголовочного блока запроса HTTP/1.1 — чистая функция без сети,
 * без QObject и без обращений к конфигу/кошельку.
 *
 * Вынесено из ProxyConnection::handleHead() ради фаззинга
 * (tests/audit/fuzz-head.cpp) и юнит-проверок парсера.
 *
 * Политика — fail closed: всё, что расходится с RFC 9110/9112 (управляющие
 * байты, obs-fold, дубликаты Content-Length, TE+CL, пустые/невалидные имена),
 * отбраковывается с 400. Это не даёт апстриму увидеть не то же самое, что
 * видел прокси (защита от CL.TE/TE.CL-десинка).
 */
struct ProxyHeadRequest {
    QByteArray method;
    QByteArray path;
    QByteArray version = QByteArrayLiteral("HTTP/1.1");
    QList<QPair<QByteArray, QByteArray>> headers; // как пришли, с повторами
    QList<QByteArray> connectionOptions;          // токены из Connection (lowercase)
    QByteArray token;                             // Bearer приоритетнее X-Api-Key
    int errorStatus = 0;                          // 0 — ок; иначе HTTP-код отказа
    QByteArray errorDetail;                       // короткая причина для лога
    bool chunked = false;                         // Transfer-Encoding: ...chunked
    bool hasContentLength = false;
    qint64 contentLength = -1;
};

/**
 * @param head         блок заголовков, включая завершающие "\r\n\r\n"
 * @param maxBodyBytes лимит Content-Length (больше — errorStatus = 413)
 *
 * Коды отказа: 400 (синтаксис/дубликаты/TE+CL), 413 (тело больше лимита),
 * 414 (URI длиннее 8192), 431 (больше 100 заголовков), 501 (метод длиннее 32).
 * При любом отказе все поля запроса очищаются.
 */
ProxyHeadRequest parseProxyHead(const QByteArray &head, qint64 maxBodyBytes);

#endif // PROXYHEAD_H
