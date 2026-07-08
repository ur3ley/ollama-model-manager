#ifndef I18N_H
#define I18N_H

#include <QObject>
#include <QHash>
#include <QString>
#include <QStringList>

class I18n : public QObject
{
    Q_OBJECT
public:
    static I18n *instance();

    void setLanguagesDir(const QString &dir);
    QString languagesDir() const { return m_dir; }

    QStringList availableCodes() const;
    QString displayName(const QString &code) const;

    QString currentCode() const { return m_code; }
    bool setLanguage(const QString &code);

    QString t(const QString &key) const;

signals:
    void languageChanged();

private:
    explicit I18n(QObject *parent = nullptr);
    static QHash<QString, QString> parseIni(const QString &path);

    QString m_dir;
    QString m_code;
    QHash<QString, QString> m_current;
    QHash<QString, QString> m_fallback;
};

inline QString T(const QString &key) { return I18n::instance()->t(key); }

#endif // I18N_H
