#include "translator.h"
#include "config.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>
#include <qdebug.h>
#include <qlogging.h>

Translator::Translator(QObject *parent)
    : QObject(parent)
    , networkManager(new QNetworkAccessManager(this))
{
    connect(networkManager, &QNetworkAccessManager::finished, this, &Translator::handleNetworkReply);
}

void Translator::translateByGoogle(const QString &sourceText)
{
    const QString targetLang = Config::targetLanguage().isEmpty() ? QStringLiteral("DE") : Config::targetLanguage();

    if (!apiKey.isEmpty()) {
        QUrl url(QStringLiteral("https://translation.googleapis.com/language/translate/v2"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("key"), apiKey);
        query.addQueryItem(QStringLiteral("q"), sourceText);
        query.addQueryItem(QStringLiteral("target"), targetLang);
        query.addQueryItem(QStringLiteral("format"), QStringLiteral("text"));
        url.setQuery(query);
        QNetworkRequest request(url);
        networkManager->get(request);
        return;
    }

    QUrl url(QStringLiteral("https://translate.googleapis.com/translate_a/single"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("gtx"));
    query.addQueryItem(QStringLiteral("ie"), QStringLiteral("UTF-8"));
    query.addQueryItem(QStringLiteral("sl"), QStringLiteral("auto"));
    query.addQueryItem(QStringLiteral("tl"), targetLang);
    query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
    query.addQueryItem(QStringLiteral("q"), sourceText);
    url.setQuery(query);
    QNetworkRequest request(url);
    networkManager->get(request);
}

void Translator::translateByDeepL(const QString &sourceText)
{
    const QString authKey = apiKey.trimmed();
    if (authKey.isEmpty()) {
        Q_EMIT translationReady(QStringLiteral("Error: DeepL API key is missing"));
        return;
    }

    const QString targetLang = Config::targetLanguage().isEmpty() ? QStringLiteral("DE") : Config::targetLanguage();

    const bool isFreeApiKey = authKey.endsWith(QStringLiteral(":fx"));
    QUrl url(isFreeApiKey ? QStringLiteral("https://api-free.deepl.com/v2/translate") : QStringLiteral("https://api.deepl.com/v2/translate"));

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("text"), sourceText);
    query.addQueryItem(QStringLiteral("target_lang"), targetLang);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Authorization", "DeepL-Auth-Key " + authKey.toUtf8());
    networkManager->post(request, query.query(QUrl::FullyEncoded).toUtf8());
}

void Translator::translate(const QString &sourceText)
{
    engine = static_cast<TranslatorEngine>(Config::translatorEngine());
    if (engine == TranslatorEngine::DeepL) {
        apiKey = Config::deeplApiKey();
        translateByDeepL(sourceText);
    } else {
        apiKey = Config::googleApiKey();
        translateByGoogle(sourceText);
    }
}

void Translator::handleNetworkReply(QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::NoError) {
        QByteArray response = reply->readAll();
        QJsonDocument jsonDoc = QJsonDocument::fromJson(response);
        QString translatedText;
        if (engine == TranslatorEngine::DeepL) {
            QJsonObject jsonObject = jsonDoc.object();
            QJsonArray translationsArray = jsonObject[QStringLiteral("translations")].toArray();
            if (!translationsArray.isEmpty()) {
                translatedText = translationsArray[0].toObject()[QStringLiteral("text")].toString();
            }
        } else if (engine == TranslatorEngine::Google) {
            qDebug() << "Translation response: " << jsonDoc.toJson(QJsonDocument::Indented);
            if (!apiKey.isEmpty()) {
                const QJsonObject jsonObject = jsonDoc.object();
                const QJsonArray translationsArray = jsonObject[QStringLiteral("data")].toObject()[QStringLiteral("translations")].toArray();
                for (const QJsonValue &translation : translationsArray) {
                    translatedText += translation.toObject()[QStringLiteral("translatedText")].toString();
                }
            } else {
                QJsonArray jsonArray = jsonDoc.array();
                QJsonArray translationsArray = jsonArray[0].toArray();
                QJsonArray hyperArray;
                for (int i = 0; i < translationsArray.size(); i++) {
                    hyperArray = translationsArray[i].toArray();
                    translatedText += hyperArray[0].toString();
                }
            }
        }
        Q_EMIT translationReady(translatedText);
    } else {
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString response = QString::fromUtf8(reply->readAll()).trimmed();
        QString errorMessage = QStringLiteral("Error");
        if (statusCode > 0) {
            errorMessage += QStringLiteral(" %1").arg(statusCode);
        }
        errorMessage += QStringLiteral(": ") + reply->errorString();
        if (!response.isEmpty()) {
            errorMessage += QStringLiteral(" - ") + response;
        }
        Q_EMIT translationReady(errorMessage);
    }
    reply->deleteLater();
}

#include "moc_translator.cpp"
