#include "epubschemehandler.h"

#include <QBuffer>
#include <QDebug>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QWebEngineUrlRequestJob>
#include <qhashfunctions.h>
#include <qlogging.h>
EpubSchemeHandler::EpubSchemeHandler(QObject *parent)
    : QWebEngineUrlSchemeHandler(parent)
{
}

void EpubSchemeHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    qDebug() << "EpubSchemeHandler::requestStarted" << job->requestUrl();
    const QUrl epubUrl = job->requestUrl();

    const QString bookId = epubUrl.host();

    QUrlQuery in(epubUrl);

    QUrl httpUrl(QStringLiteral("http://localhost:8080/preview"));

    QUrlQuery out;
    out.addQueryItem(QStringLiteral("book"), bookId);

    if (in.hasQueryItem(QStringLiteral("cfi")))
        out.addQueryItem(QStringLiteral("cfi"), in.queryItemValue(QStringLiteral("cfi")));

    httpUrl.setQuery(out);

    QNetworkReply *reply = m_network.get(QNetworkRequest(httpUrl));

    connect(reply, &QNetworkReply::finished, job, [job, reply]() {
        QByteArray data = reply->readAll();

        auto *buffer = new QBuffer(job);
        buffer->setData(data);
        buffer->open(QIODevice::ReadOnly);

        job->reply("text/html", buffer);

        reply->deleteLater();
    });
}
