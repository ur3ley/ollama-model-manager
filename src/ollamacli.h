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
        QString renderer;
        QString parser;
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

    // Modelfile parsing helpers (public for the offline self-test).
    static QString extractFromBase(const QString &modelfileContent);
    static QString extractDirective(const QString &modelfileContent, const QString &directive);

    void createModelAsync(const QString &name, const QString &modelfilePath);
    void deleteModelAsync(const QString &name);
    void copyModelAsync(const QString &oldName, const QString &newName);

    /** Pull a model from the Ollama registry; progress comes as CLI output. */
    void pullModelAsync(const QString &name);
    void cancelPull();
    bool isPulling() const;

signals:
    void createFinished(bool success, const QString &output);
    void deleteFinished(bool success, const QString &output);
    void copyFinished(bool success, const QString &output);

    void pullProgress(const QString &status, int percent);
    void pullFinished(bool success, const QString &output);

private:
    QString runCommand(const QStringList &args, int timeoutMs = 15000, bool *ok = nullptr);
    QList<ModelInfo> parseListOutput(const QString &output);
    ModelParams parseShowOutput(const QString &output);

    QString m_ollamaPath;
    QProcess *m_pull = nullptr;
    QString m_pullBuffer;
    QString m_lastPullStatus;
    int m_lastPullPercent = -2;
};

#endif // OLLAMACLI_H
