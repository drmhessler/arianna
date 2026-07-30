#pragma once

#include <QNetworkAccessManager>
#include <QWebEngineUrlSchemeHandler>

class EpubSchemeHandler : public QWebEngineUrlSchemeHandler
{
    Q_OBJECT

public:
    explicit EpubSchemeHandler(QObject *parent = nullptr);

    void requestStarted(QWebEngineUrlRequestJob *job) override;

private:
    QNetworkAccessManager m_network;
};