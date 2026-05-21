#pragma once

#include <QHttpServer>
#include <QHttpServerResponse>
#include <QMap>
#include <QSet>
#include <QString>

#include <memory>

#include "epubcontainer.h"
class BookServer
{
public:
    explicit BookServer(const QString &token = QString());
    ~BookServer();
    using ResourceMap = QMap<QString, QString>;
    bool isRunning() const;
    void addSessionToken(const QString &token);
    void removeToken(const QString &token);
    ResourceMap makeResourceMap(const QSet<QString> &servedFiles, const QString &identifier);

private:
    bool isValidToken(const QString &token) const;
    std::shared_ptr<EPubContainer> containerForIdentifier(const QString &identifier);
    struct ServedResource {
        QString identifier;
        QString path;
    };
    QHttpServer server;
    QString m_serverToken;
    QSet<QString> m_validTokens;
    QHash<QString, ServedResource> m_resourceByUuid;
    QMap<QString, QSet<QString>> m_servedFilesByIdentifier;
    QMap<QString, QMap<QString, QString>> m_resourceMapByIdentifier;
    QMap<QString, std::shared_ptr<EPubContainer>> m_containerCache;
    bool m_running = false;
};
