#ifndef TRANSLATOR_H
#define TRANSLATOR_H

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <qqmlintegration.h>

class Translator : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit Translator(QObject *parent = nullptr);
    virtual ~Translator() = default;

    Q_INVOKABLE void translate(const QString &sourceText);

    enum TranslatorEngine {
        Google = 0,
        DeepL = 1
    };
    Q_ENUM(TranslatorEngine)

Q_SIGNALS:
    void translationReady(const QString &translatedText);

private Q_SLOTS:
    void handleNetworkReply(QNetworkReply *reply);

private:
    QNetworkAccessManager *networkManager;
    QString apiKey;
    TranslatorEngine engine; // 0=Google, 1=DeepL
    void translateByDeepL(const QString &sourceText);
    void translateByGoogle(const QString &sourceText);
};

#endif // TRANSLATOR_H
