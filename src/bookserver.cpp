// SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "bookserver.h"
#include "bookdatabase.h"
#include "categoryentriesmodel.h"

#include <QAbstractSocket>
#include <QFileInfo>
#include <QTcpServer>

#include <KArchiveDirectory>
#include <KArchiveFile>
#include <KZip>

#include <QBuffer>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopedPointer>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <qdeadlinetimer.h>
#include <qdebug.h>

static void writeDiscoveryFile(quint16 port, const QString &serverToken)
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/arianna-bookserver.json");

    QJsonObject obj;
    obj.insert(QStringLiteral("baseUrl"), QStringLiteral("http://127.0.0.1:%1").arg(port));
    obj.insert(QStringLiteral("serverToken"), serverToken);
    obj.insert(QStringLiteral("pid"), QCoreApplication::applicationPid());

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "Could not write BookServer discovery file:" << path;
        return;
    }

    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    file.close();

    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    qWarning() << "BookServer discovery file:" << path;
}

static bool isServerServedMimeType(const QByteArray &mime)
{
    // Nur große/problemlose Streaming-Medien externalisieren

    return mime == QByteArrayLiteral("video/mp4") || mime == QByteArrayLiteral("audio/mpeg");
}

static QSet<QString> makeServedFiles(const std::shared_ptr<EPubContainer> &container)
{
    QSet<QString> servedFiles;

    if (!container) {
        return servedFiles;
    }

    const QHash<QString, EpubItem> &manifest = container->manifestItems();

    for (auto it = manifest.constBegin(); it != manifest.constEnd(); ++it) {
        const EpubItem &item = it.value();

        if (isServerServedMimeType(item.mimetype)) {
            servedFiles.insert(QDir::cleanPath(item.path));
        }
    }

    return servedFiles;
}

static bool shouldStripPath(const QString &path)
{
    const QString lower = path.toLower();

    return lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg")) || lower.endsWith(QStringLiteral(".png"))
        || lower.endsWith(QStringLiteral(".webp")) || lower.endsWith(QStringLiteral(".gif")) || lower.endsWith(QStringLiteral(".mp3"))
        || lower.endsWith(QStringLiteral(".ogg")) || lower.endsWith(QStringLiteral(".opus")) || lower.endsWith(QStringLiteral(".m4a"))
        || lower.endsWith(QStringLiteral(".mp4")) || lower.endsWith(QStringLiteral(".webm"));
}

static void copyDirectoryStripped(KZip &outZip, const KArchiveDirectory *dir, const QString &prefix, QSet<QString> *strippedFiles)
{
    const QStringList entries = dir->entries();

    for (const QString &entryName : entries) {
        const KArchiveEntry *entry = dir->entry(entryName);

        if (!entry) {
            continue;
        }

        const QString fullPath = prefix.isEmpty() ? entryName : prefix + QStringLiteral("/") + entryName;
        if (entry->isDirectory()) {
            auto subdir = dynamic_cast<const KArchiveDirectory *>(entry);

            if (subdir) {
                copyDirectoryStripped(outZip, subdir, fullPath, strippedFiles);
            }

            continue;
        }

        auto file = dynamic_cast<const KArchiveFile *>(entry);

        if (!file) {
            continue;
        }

        if (shouldStripPath(fullPath)) {
            if (strippedFiles) {
                strippedFiles->insert(QDir::cleanPath(fullPath));
            }

            qDebug() << "Stripping:" << fullPath;

            continue;
        }

        QScopedPointer<QIODevice> dev(file->createDevice());

        if (!dev) {
            continue;
        }

        outZip.writeFile(fullPath, dev->readAll());
    }
}
static QString makeResourceUrl(const QString &identifier, const QString &resourceUuid)
{
    return QStringLiteral("http://127.0.0.1:45961/") + QString::fromUtf8(QUrl::toPercentEncoding(identifier)) + QStringLiteral("/r/")
        + QString::fromUtf8(QUrl::toPercentEncoding(resourceUuid));
}

static QString requestHeaderValue(const QHttpServerRequest &request, const QByteArray &name)
{
    const QByteArray exactValue = request.value(name);
    if (!exactValue.isEmpty()) {
        return QString::fromUtf8(exactValue);
    }

    for (const auto &[headerName, headerValue] : request.headers().toListOfPairs()) {
        if (headerName.compare(name, Qt::CaseInsensitive) == 0) {
            return QString::fromUtf8(headerValue);
        }
    }

    return {};
}

static QString requestToken(const QHttpServerRequest &request)
{
    const QString headerToken = requestHeaderValue(request, QByteArrayLiteral("X-Arianna-Session-Token"));
    if (!headerToken.isEmpty()) {
        return headerToken;
    }

    return request.query().queryItemValue(QStringLiteral("token"));
}

static bool isAllowedCorsOrigin(const QByteArray &origin)
{
    return origin == QByteArrayLiteral("qrc:") || origin == QByteArrayLiteral("foliate://") || origin == QByteArrayLiteral("foliate:");
}

static void addCorsHeaders(const QHttpServerRequest &request, QHttpServerResponse &response)
{
    const QByteArray origin = request.value("Origin");
    if (!isAllowedCorsOrigin(origin)) {
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    auto headers = response.headers();
    headers.replaceOrAppend("Access-Control-Allow-Origin", origin);
    headers.replaceOrAppend("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    headers.replaceOrAppend("Access-Control-Allow-Headers", "X-Arianna-Session-Token, X-Arianna-Server-Token, Range, Content-Type");
    headers.replaceOrAppend("Access-Control-Expose-Headers", "Accept-Ranges, Content-Length, Content-Range");
    headers.replaceOrAppend("Vary", "Origin");
    response.setHeaders(headers);
#else
    response.setHeader("Access-Control-Allow-Origin", origin);
    response.setHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    response.setHeader("Access-Control-Allow-Headers", "X-Arianna-Session-Token, X-Arianna-Server-Token, Range, Content-Type");
    response.setHeader("Access-Control-Expose-Headers", "Accept-Ranges, Content-Length, Content-Range");
    response.setHeader("Vary", "Origin");
#endif
}

static QHttpServerResponse corsPreflightResponse(const QHttpServerRequest &request)
{
    Q_UNUSED(request)
    QHttpServerResponse response(QHttpServerResponder::StatusCode::NoContent);
    return response;
}

std::shared_ptr<EPubContainer> BookServer::containerForIdentifier(const QString &identifier)
{
    if (m_containerCache.contains(identifier)) {
        qDebug() << "Container aus Cache:" << identifier;
        return m_containerCache.value(identifier);
    }

    auto entry = BookDatabase::self().loadEntryByIdentifier(identifier);

    if (!entry) {
        qWarning() << "Kein BookEntry für Identifier:" << identifier;
        return {};
    }

    auto container = std::make_shared<EPubContainer>(nullptr);

    if (!container->openFile(entry->filename)) {
        qWarning() << "EPUB konnte nicht geöffnet werden:" << entry->filename;
        return {};
    }

    m_containerCache.insert(identifier, container);

    qDebug() << "Container aus DB geladen:" << identifier << entry->filename;

    return container;
}

static QString serverTokenFromRequest(const QHttpServerRequest &request)
{
    const QString headerToken = requestHeaderValue(request, QByteArrayLiteral("X-Arianna-Server-Token"));
    if (!headerToken.isEmpty()) {
        return headerToken;
    }

    return request.query().queryItemValue(QStringLiteral("serverToken"));
}

BookServer::BookServer(const QString &serverToken)
    : m_serverToken(serverToken)
{
    addSessionToken(m_serverToken);

    server.route(QStringLiteral("/static/background.webp"), [](const QHttpServerRequest &request) {
        Q_UNUSED(request)
        qDebug() << "backgruound serverd";
        QFile file(QStringLiteral(":/qt/qml/org/kde/arianna/qml/background.webp"));

        // Falls die Datei in einem Prefix liegt, z.B.:
        // QFile file(QStringLiteral(":/qt/qml/org/kde/arianna/qml/background.webp"));

        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "Unable to open qrc background.webp";
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const QByteArray data = file.readAll();

        QHttpServerResponse response(QByteArrayLiteral("image/webp"), data);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        auto headers = response.headers();
        headers.append("Cache-Control", "public, max-age=3600");
        response.setHeaders(headers);
#else
    response.setHeader("Cache-Control", "public, max-age=3600");
#endif

        return response;
    });

    server.route(QStringLiteral("/session"), QHttpServerRequest::Method::Options, [](const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });

    server.route(QStringLiteral("/session"), QHttpServerRequest::Method::Post, [this](const QHttpServerRequest &request) {
        qDebug() << "Request for session token" << serverTokenFromRequest(request);
        if (serverTokenFromRequest(request) != m_serverToken) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        const QString sessionToken = QUuid::createUuid().toString(QUuid::WithoutBraces);
        addSessionToken(sessionToken);

        QJsonObject obj;
        obj.insert(QStringLiteral("sessionToken"), sessionToken);
        obj.insert(QStringLiteral("headerName"), QStringLiteral("X-Arianna-Session-Token"));

        return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    server.route(QStringLiteral("/<arg>/book.epub"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/book"), QHttpServerRequest::Method::Options, [](const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });

    server.route(QStringLiteral("/book"), [this](const QHttpServerRequest &request) {
        qDebug() << "Request for direct book URL:" << request.url() << "request token" << requestToken(request);
        const QString token = requestToken(request);

        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        const QString bookUrl = QUrl::fromPercentEncoding(request.query().queryItemValue(QStringLiteral("url")).toUtf8());
        QUrl parsedBookUrl = QUrl::fromUserInput(bookUrl);
        if (!parsedBookUrl.isLocalFile() && bookUrl.startsWith(QStringLiteral("file://"))) {
            parsedBookUrl = QUrl::fromEncoded(bookUrl.toUtf8());
        }

        QString filename = parsedBookUrl.isLocalFile() ? parsedBookUrl.toLocalFile() : bookUrl;
        if (filename.startsWith(QStringLiteral("file://"))) {
            filename = filename.mid(7);
        }

        qDebug() << "Resolved direct book path:" << filename;

        if (filename.isEmpty() || !QFileInfo::exists(filename)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        auto container = std::make_shared<EPubContainer>(nullptr);

        if (!container->openFile(filename)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const QByteArray data = container->createServerReadyEpub({});

        if (data.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::InternalServerError};
        }

        QHttpServerResponse response(QByteArrayLiteral("application/epub+zip"), data);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        auto headers = response.headers();
        headers.append("Content-Disposition", "inline; filename=\"book.epub\"");
        headers.append("Content-Length", QByteArray::number(data.size()));
        headers.append("Cache-Control", "no-store");
        response.setHeaders(headers);
#else
        response.setHeader("Content-Disposition", "inline; filename=\"book.epub\"");
        response.setHeader("Content-Length", QByteArray::number(data.size()));
        response.setHeader("Cache-Control", "no-store");
#endif

        return response;
    });

    server.route(QStringLiteral("/<arg>/book.epub"), [this](const QString &identifier, const QHttpServerRequest &request) {
        qDebug() << "Request for book URL:" << request.url() << "request token" << requestToken(request);
        const QString token = requestToken(request);

        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        auto container = containerForIdentifier(identifier);
        if (!container) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }
        const QSet<QString> servedFiles = makeServedFiles(container);
        m_servedFilesByIdentifier[identifier] = servedFiles;

        const ResourceMap resourceMap = makeResourceMap(servedFiles, identifier);
        const QByteArray data = container->createServerReadyEpub(resourceMap);

        qDebug() << "Served files map for identifier:" << identifier << resourceMap;
        if (data.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::InternalServerError};
        }

        QHttpServerResponse response(QByteArrayLiteral("application/epub+zip"), data);
        // qDebug() << "Serving file directly:" << container->filename();
        // QHttpServerResponse response = QHttpServerResponse::fromFile(container->filename());
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        auto headers = response.headers();
        // headers.append("Access-Control-Allow-Origin", "*");
        headers.append("Content-Disposition", "inline; filename=\"book.epub\"");
        headers.append("Content-Length", QByteArray::number(data.size()));
        headers.append("Cache-Control", "no-store");
        response.setHeaders(headers);
#else
      // response.setHeader("Access-Control-Allow-Origin", "*");
    response.setHeader("Content-Disposition", "inline; filename=\"book.epub\"");
    response.setHeader("Content-Length", QByteArray::number(data.size()));
    response.setHeader("Cache-Control", "no-store");
#endif

        return response;
    });
    // Route for book resources
    server.route(QStringLiteral("/<arg>/r/<arg>"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QString & /*resourceUuid*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/r/<arg>"), [this](const QString &identifier, const QString &resourceUuid, const QHttpServerRequest &request) {
        qDebug() << "Request for resource URL:" << request.url() << "with token:" << requestToken(request);

        const QString token = requestToken(request);
        if (!isValidToken(token)) {
            qDebug() << "invalid token";
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        if (!m_resourceByUuid.contains(resourceUuid)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const ServedResource served = m_resourceByUuid.value(resourceUuid);

        if (served.identifier != identifier) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Forbidden};
        }

        const QString cleanPath = QDir::cleanPath(served.path);

        if (!m_servedFilesByIdentifier.value(identifier).contains(cleanPath)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Forbidden};
        }

        auto container = containerForIdentifier(identifier);
        if (!container) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const QByteArray data = container->readData(cleanPath);
        if (data.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const qint64 totalSize = data.size();
        const QByteArray rangeHeader = request.value("Range");

        qint64 start = 0;
        qint64 end = totalSize - 1;
        bool partial = false;

        if (rangeHeader.startsWith("bytes=")) {
            const QByteArray range = rangeHeader.mid(strlen("bytes="));
            const QList<QByteArray> parts = range.split('-');

            bool okStart = false;
            bool okEnd = false;

            if (!parts.value(0).isEmpty()) {
                start = parts.value(0).toLongLong(&okStart);
            }

            if (parts.size() > 1 && !parts.value(1).isEmpty()) {
                end = parts.value(1).toLongLong(&okEnd);
            }

            if (okStart) {
                if (!okEnd) {
                    end = totalSize - 1;
                }

                if (start >= 0 && end >= start && end < totalSize) {
                    partial = true;
                } else {
                    return QHttpServerResponse{QHttpServerResponder::StatusCode::RequestRangeNotSatisfiable};
                }
            }
        }
        qDebug() << "Serving resource:" << cleanPath << "size:" << totalSize << "partial:" << partial << "range:" << start << "-" << end;
        const QByteArray body = partial ? data.mid(start, end - start + 1) : data;

        QMimeDatabase db;
        const QString mime = db.mimeTypeForFile(cleanPath, QMimeDatabase::MatchExtension).name();

        QHttpServerResponse response(mime.isEmpty() ? QByteArrayLiteral("application/octet-stream") : mime.toUtf8(),
                                     body,
                                     partial ? QHttpServerResponder::StatusCode::PartialContent : QHttpServerResponder::StatusCode::Ok);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        auto headers = response.headers();
        headers.append("Accept-Ranges", "bytes");
        headers.append("Cache-Control", "public, max-age=3600");
        headers.append("Content-Length", QByteArray::number(body.size()));

        if (partial) {
            headers.append("Content-Range",
                           QByteArray("bytes ") + QByteArray::number(start) + "-" + QByteArray::number(end) + "/" + QByteArray::number(totalSize));
        }

        response.setHeaders(headers);
#else
response.setHeader("Accept-Ranges", "bytes");
response.setHeader("Cache-Control", "public, max-age=3600");
response.setHeader("Content-Length", QByteArray::number(body.size()));

if (partial) {
    response.setHeader(
        "Content-Range",
        QByteArray("bytes ")
            + QByteArray::number(start)
            + "-"
            + QByteArray::number(end)
            + "/"
            + QByteArray::number(totalSize));
}
#endif

        return response;
    });

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    server.addAfterRequestHandler(&server, [](const QHttpServerRequest &request, QHttpServerResponse &resp) {
        addCorsHeaders(request, resp);
    });
#else
    server.afterRequest([](QHttpServerResponse &&resp) {
        resp.setHeader("Access-Control-Allow-Origin", "*");
        resp.setHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        resp.setHeader("Access-Control-Allow-Headers", "X-Arianna-Session-Token, X-Arianna-Server-Token, Range, Content-Type");
        resp.setHeader("Access-Control-Expose-Headers", "Accept-Ranges, Content-Length, Content-Range");
        return std::move(resp);
    });
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    auto tcpserver = std::make_unique<QTcpServer>();
    if (!tcpserver->listen(QHostAddress::LocalHost, 45961)) {
        if (tcpserver->serverError() == QAbstractSocket::AddressInUseError) {
            qWarning().noquote() << "BookServer already running";
        } else {
            qWarning().noquote() << "Server failed to listen on a port:" << tcpserver->errorString();
        }
        return;
    }
    if (!server.bind(tcpserver.get())) {
        qWarning().noquote() << "Server failed to bind to the listening socket.";
        return;
    }
    quint16 port = tcpserver->serverPort();
    auto s = tcpserver.release();
    Q_UNUSED(s);
#else
    const auto port = server.listen(QHostAddress::LocalHost, 45961);
    if (!port) {
        qWarning().noquote() << "BookServer already running";
        return;
    }
#endif

    m_running = true;
    qWarning() << QStringLiteral("BookServer running on http://127.0.0.1:%1/ (Press CTRL+C to quit)").arg(port);
    writeDiscoveryFile(port, m_serverToken);
}

bool BookServer::isRunning() const
{
    return m_running;
}

void BookServer::addSessionToken(const QString &token)
{
    if (token.isEmpty()) {
        return;
    }

    m_validTokens.insert(token);

    qDebug() << "BookServer session token added:" << token;
}

void BookServer::removeToken(const QString &token)
{
    m_validTokens.remove(token);

    qDebug() << "BookServer token removed:" << token;
}

bool BookServer::isValidToken(const QString &token) const
{
    if (token.isEmpty()) {
        return false;
    }

    if (m_validTokens.contains(token)) {
        qDebug() << "BookServer token valid:" << token;
        return true;
    }
    qWarning() << "BookServer token invalid:" << token;
    return false;
}

ResourceMap BookServer::makeResourceMap(const QSet<QString> &servedFiles, const QString &identifier)
{
    ResourceMap map;

    for (const QString &path : servedFiles) {
        const QString cleanPath = QDir::cleanPath(path);

        const QString resourceUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

        m_resourceByUuid.insert(resourceUuid, ServedResource{identifier, cleanPath});

        map.insert(cleanPath, makeResourceUrl(identifier, resourceUuid));
    }

    return map;
}

BookServer::~BookServer()
{
    if (!m_running) {
        return;
    }

    const QString path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/arianna-bookserver.json");

    if (QFile::exists(path)) {
        QFile::remove(path);

        qDebug() << "Removed BookServer discovery file:" << path;
    }
}
