#ifndef OLLAMACLI_H
#define OLLAMACLI_H

#include <QObject>
#include <QProcess>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

class OllamaCli : public QObject
{
    Q_OBJECT
public:
    struct ModelInfo {
        QString name;
        QString id;
        QString size;
        QString modified;
    };

    struct ModelParams {
        QString architecture;
        QString paramCount;
        int contextLength = 0;
        QString quantization;
        QMap<QString, QString> parameters;
        QString systemPrompt;
        QString templateStr;
        QString fromBase;
        QStringList capabilities;
    };

    explicit OllamaCli(QObject *parent = nullptr);

    bool isAvailable() const;
    QString ollamaPath() const;

    QList<ModelInfo> listModels();
    ModelParams showModel(const QString &name);
    QString showModelfile(const QString &name);

    QString getModelFromBlob(const QString &name);
    QString findOtherModelSharingBlob(const QString &blob, const QString &excludeName);
    int countBlobReferences(const QString &blob);

    void createModelAsync(const QString &name, const QString &modelfilePath);
    void deleteModelAsync(const QString &name);
    void copyModelAsync(const QString &oldName, const QString &newName);

signals:
    void createFinished(bool success, const QString &output);
    void deleteFinished(bool success, const QString &output);
    void copyFinished(bool success, const QString &output);

private:
    QString runCommand(const QStringList &args, int timeoutMs = 15000, bool *ok = nullptr);
    QList<ModelInfo> parseListOutput(const QString &output);
    ModelParams parseShowOutput(const QString &output);
    static QString extractFromBase(const QString &modelfileContent);

    QString m_ollamaPath;
};

#endif // OLLAMACLI_H
