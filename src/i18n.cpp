#include "i18n.h"
#include <QFile>
#include <QDir>
#include <QTextStream>

I18n *I18n::instance()
{
    static I18n inst;
    return &inst;
}

I18n::I18n(QObject *parent)
    : QObject(parent)
{
}

void I18n::setLanguagesDir(const QString &dir)
{
    m_dir = dir;
    m_fallback = parseIni(m_dir + "/en.ini");
}

QStringList I18n::availableCodes() const
{
    QStringList codes;
    QDir dir(m_dir);
    const QStringList files = dir.entryList({"*.ini"}, QDir::Files, QDir::Name);
    for (const QString &f : files) {
        QString code = f;
        code.chop(4); // ".ini"
        if (!code.isEmpty())
            codes << code;
    }
    return codes;
}

QString I18n::displayName(const QString &code) const
{
    QHash<QString, QString> map = parseIni(m_dir + "/" + code + ".ini");
    QString name = map.value("meta.name");
    return name.isEmpty() ? code : name;
}

bool I18n::setLanguage(const QString &code)
{
    QHash<QString, QString> map = parseIni(m_dir + "/" + code + ".ini");
    if (map.isEmpty())
        return false;
    m_current = map;
    m_code = code;
    emit languageChanged();
    return true;
}

QString I18n::t(const QString &key) const
{
    auto it = m_current.find(key);
    if (it != m_current.end())
        return it.value();
    auto f = m_fallback.find(key);
    if (f != m_fallback.end())
        return f.value();
    return key;
}

QHash<QString, QString> I18n::parseIni(const QString &path)
{
    QHash<QString, QString> map;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return map;

    QTextStream in(&file);
    in.setEncoding(QStringConverter::Utf8);

    QString section;
    while (!in.atEnd()) {
        QString line = in.readLine();
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty() || trimmed.startsWith('#') || trimmed.startsWith(';'))
            continue;

        if (trimmed.startsWith('[') && trimmed.endsWith(']')) {
            section = trimmed.mid(1, trimmed.length() - 2).trimmed();
            continue;
        }

        int eq = trimmed.indexOf('=');
        if (eq < 0)
            continue;

        QString key = trimmed.left(eq).trimmed();
        QString value = trimmed.mid(eq + 1).trimmed();
        if (key.isEmpty())
            continue;

        value.replace("\\n", "\n");
        value.replace("\\t", "\t");

        QString fullKey = section.isEmpty() ? key : section + "." + key;
        map.insert(fullKey, value);
    }
    return map;
}
