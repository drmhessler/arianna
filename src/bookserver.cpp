// SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
// SPDX-License-Identifier: LGPL-2.0-or-later

#include "bookserver.h"
#include "bookdatabase.h"
#include "categoryentriesmodel.h"
#include "config.h"
#include "referencestore.h"

#include <QAbstractSocket>
#include <QFileInfo>
#include <QMimeDatabase>
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopedPointer>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <qcoreapplication.h>
#include <qdeadlinetimer.h>
#include <qdebug.h>
#include <qdir.h>

#include <algorithm>

static QString discoveryFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/arianna-bookserver.json");
}

static void removeDiscoveryFile()
{
    const QString path = discoveryFilePath();
    if (QFile::exists(path))
        QFile::remove(path);
}

static void writeDiscoveryFile(quint16 port, const QString &serverToken)
{
    const QString path = discoveryFilePath();

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

static QString anchoredEpubPath(const QString &filename, const QString &bookId)
{
    QFileInfo fileInfo(filename);
    QString fileStem = bookId.trimmed();
    if (fileStem.isEmpty()) {
        fileStem = fileInfo.completeBaseName();
    }

    fileStem.replace(QRegularExpression(QStringLiteral("[/\\\\]")), QStringLiteral("_"));
    return fileInfo.dir().filePath(fileStem + QStringLiteral(".anchored.epub"));
}

static QString manualBookrefAnchorOpenTag(const QString &sourceAnchorId, const QString &targetLocation, const bool isCrossReference)
{
    QString tag = QStringLiteral("<a id=\"%1\" data-role=\"anchor\"").arg(sourceAnchorId.toHtmlEscaped());
    if (isCrossReference) {
        tag += QStringLiteral(" data-anchor-type=\"crossref\"");
    } else if (!targetLocation.isEmpty()) {
        tag += QStringLiteral(" href=\"%1\"").arg(targetLocation.toHtmlEscaped());
    }
    tag += QStringLiteral(">");
    return tag;
}

static bool findReferenceableTarget(const std::shared_ptr<EPubContainer> &container, const QString &refOrLocation, TargetAnchorInfo *target)
{
    if (!container || refOrLocation.isEmpty()) {
        return false;
    }

    for (const TargetAnchorInfo &candidate : container->referenceableTargets()) {
        if (candidate.ref == refOrLocation || candidate.location == refOrLocation) {
            if (target) {
                *target = candidate;
            }
            return true;
        }
    }

    return false;
}

static QString deliveryQueryValue(const QHttpServerRequest &request, const QStringList &names, const QString &fallback)
{
    const auto query = request.query();
    for (const QString &name : names) {
        const QString value = query.queryItemValue(name).trimmed();
        if (!value.isEmpty()) {
            return value;
        }
    }

    return fallback;
}

static QString normalizeResourceMode(QString mode)
{
    mode = mode.trimmed().toLower();
    if (mode == QStringLiteral("include") || mode == QStringLiteral("inline") || mode == QStringLiteral("off") || mode == QStringLiteral("none")) {
        return QStringLiteral("include");
    }
    if (mode == QStringLiteral("outsource") || mode == QStringLiteral("server") || mode == QStringLiteral("external")) {
        return QStringLiteral("outsource");
    }
    return QStringLiteral("auto");
}

static QString normalizeReferencingMode(QString mode)
{
    mode = mode.trimmed().toLower();
    if (mode == QStringLiteral("include")) {
        return QStringLiteral("include");
    }
    if (mode == QStringLiteral("none") || mode == QStringLiteral("off")) {
        return QStringLiteral("none");
    }
    return QStringLiteral("reader");
}

static QVector<EpubReference> epubReferencesFromVariantList(const QVariantList &values)
{
    QVector<EpubReference> references;
    references.reserve(values.size());

    for (const QVariant &value : values) {
        const QVariantMap map = value.toMap();
        EpubReference reference;
        reference.sourceAnchorId = map.value(QStringLiteral("sourceAnchorId")).toString();
        reference.targetBookId = map.value(QStringLiteral("targetBookId")).toString();
        reference.targetAnchorId = map.value(QStringLiteral("targetAnchorId")).toString();
        reference.targetLocation = map.value(QStringLiteral("targetLocation")).toString();
        reference.targetPreviewHtml = map.value(QStringLiteral("targetPreviewHtml")).toString();
        if (!reference.sourceAnchorId.isEmpty()) {
            references.append(reference);
        }
    }

    return references;
}

static bool isServerServedMimeType(const QByteArray &mime)
{
    // Nur große/problemlose Streaming-Medien externalisieren

    return mime == QByteArrayLiteral("video/mp4") || mime == QByteArrayLiteral("audio/mpeg") || mime == QByteArrayLiteral("audio/ogg")
        || mime == QByteArrayLiteral("audio/opus") || mime == QByteArrayLiteral("audio/mp4") || mime == QByteArrayLiteral("audio/webm")
        || mime == QByteArrayLiteral("video/webm");
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
        if (item.mimetype == QByteArrayLiteral("video/mp4")) {
            const QString mp4Path = QDir::cleanPath(item.path);

            servedFiles.insert(mp4Path);

            QFileInfo fi(mp4Path);

            QString syncPath = fi.completeBaseName() + QStringLiteral("-sync.json");
            qDebug() << "Looking for sync file:" << syncPath;

            // oder je nach Schema:
            // karna-amrita_1.mp4
            // -> karna-amrita_1-sync.json

            for (auto jt = manifest.constBegin(); jt != manifest.constEnd(); ++jt) {
                const EpubItem &other = jt.value();

                if (QDir::cleanPath(other.path) == syncPath) {
                    servedFiles.insert(syncPath);

                    qDebug() << "Adding sync file:" << syncPath;

                    break;
                }
            }
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
    return QStringLiteral("http://127.0.0.1:45961/") + QString::fromUtf8(QUrl::toPercentEncoding(identifier)) + QStringLiteral("/res/")
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
    headers.replaceOrAppend("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    headers.replaceOrAppend("Access-Control-Allow-Headers", "X-Arianna-Session-Token, X-Arianna-Server-Token, Range, Content-Type");
    headers.replaceOrAppend("Access-Control-Expose-Headers",
                            "Accept-Ranges, Content-Length, Content-Range, X-Arianna-Resource-Mode, X-Arianna-Referencing-Mode");
    headers.replaceOrAppend("Vary", "Origin");
    response.setHeaders(headers);
#else
    response.setHeader("Access-Control-Allow-Origin", origin);
    response.setHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    response.setHeader("Access-Control-Allow-Headers", "X-Arianna-Session-Token, X-Arianna-Server-Token, Range, Content-Type");
    response.setHeader("Access-Control-Expose-Headers", "Accept-Ranges, Content-Length, Content-Range, X-Arianna-Resource-Mode, X-Arianna-Referencing-Mode");
    response.setHeader("Vary", "Origin");
#endif
}

static QHttpServerResponse corsPreflightResponse(const QHttpServerRequest &request)
{
    Q_UNUSED(request)
    QHttpServerResponse response(QHttpServerResponder::StatusCode::NoContent);
    return response;
}

static void addBookResponseHeaders(QHttpServerResponse &response, qint64 contentLength)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    auto headers = response.headers();
    headers.replaceOrAppend("Content-Disposition", "inline; filename=\"book.epub\"");
    if (contentLength >= 0) {
        headers.replaceOrAppend("Content-Length", QByteArray::number(contentLength));
    }
    headers.replaceOrAppend("Cache-Control", "no-store");
    response.setHeaders(headers);
#else
    response.setHeader("Content-Disposition", "inline; filename=\"book.epub\"");
    if (contentLength >= 0) {
        response.setHeader("Content-Length", QByteArray::number(contentLength));
    }
    response.setHeader("Cache-Control", "no-store");
#endif
}

static void addBookDeliveryHeaders(QHttpServerResponse &response, const QString &resourceMode, const QString &referencingMode)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    auto headers = response.headers();
    headers.replaceOrAppend("X-Arianna-Resource-Mode", resourceMode.toUtf8());
    headers.replaceOrAppend("X-Arianna-Referencing-Mode", referencingMode.toUtf8());
    response.setHeaders(headers);
#else
    response.setHeader("X-Arianna-Resource-Mode", resourceMode.toUtf8());
    response.setHeader("X-Arianna-Referencing-Mode", referencingMode.toUtf8());
#endif
}

static QHttpServerResponse bookFileResponse(const QString &filename)
{
    QHttpServerResponse response = QHttpServerResponse::fromFile(filename);
    addBookResponseHeaders(response, QFileInfo(filename).size());
    return response;
}

void BookServer::clearServedResourcesForIdentifier(const QString &identifier)
{
    for (auto it = m_resourceByUuid.begin(); it != m_resourceByUuid.end();) {
        if (it.value().identifier == identifier) {
            it = m_resourceByUuid.erase(it);
        } else {
            ++it;
        }
    }

    m_servedFilesByIdentifier.remove(identifier);
    m_resourceMapByIdentifier.remove(identifier);
}

void BookServer::clearServedResourcesForSessionIdentifier(const QString &sessionToken, const QString &identifier)
{
    for (auto it = m_resourceByUuid.begin(); it != m_resourceByUuid.end();) {
        const ServedResource &served = it.value();
        if (served.identifier == identifier && served.sessionToken == sessionToken) {
            it = m_resourceByUuid.erase(it);
        } else {
            ++it;
        }
    }
}

void BookServer::registerReaderSessionForIdentifier(const QString &sessionToken, const QString &identifier)
{
    if (sessionToken.isEmpty() || identifier.isEmpty() || !m_readerSessionRefCount.contains(sessionToken)) {
        return;
    }

    m_identifiersByReaderSession[sessionToken].insert(identifier);
    m_readerSessionsByIdentifier[identifier].insert(sessionToken);
}

void BookServer::releaseReaderSessionResources(const QString &sessionToken)
{
    const QSet<QString> identifiers = m_identifiersByReaderSession.take(sessionToken);

    for (const QString &identifier : identifiers) {
        clearServedResourcesForSessionIdentifier(sessionToken, identifier);

        auto sessionsIt = m_readerSessionsByIdentifier.find(identifier);
        if (sessionsIt == m_readerSessionsByIdentifier.end()) {
            continue;
        }

        sessionsIt.value().remove(sessionToken);
        if (!sessionsIt.value().isEmpty()) {
            continue;
        }

        m_readerSessionsByIdentifier.erase(sessionsIt);
        clearServedResourcesForIdentifier(identifier);
        m_containerCache.remove(identifier);
        m_readOnlyFilesByIdentifier.remove(identifier);

        qDebug() << "BookServer cache removed for inactive identifier:" << identifier;
    }
}

std::shared_ptr<EPubContainer> BookServer::containerForIdentifier(const QString &identifier)
{
    QString bookFileName = m_readOnlyFilesByIdentifier.value(identifier);
    if (bookFileName.isEmpty()) {
        auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(identifier);

        if (!entry) {
            qWarning() << "Kein BookEntry für Identifier:" << identifier;
            m_containerCache.remove(identifier);
            clearServedResourcesForIdentifier(identifier);
            return {};
        }

        bookFileName = entry->filename;
    }

    const QString anchoredBookFileName = anchoredEpubPath(bookFileName, identifier);
    if (QFileInfo::exists(anchoredBookFileName)) {
        qDebug() << "Using anchored EPUB for identifier:" << identifier << anchoredBookFileName;
        bookFileName = anchoredBookFileName;
    }

    const QFileInfo fileInfo(bookFileName);

    if (!fileInfo.exists() || !fileInfo.isFile()) {
        qWarning() << "EPUB-Datei existiert nicht mehr:" << bookFileName;
        m_containerCache.remove(identifier);
        clearServedResourcesForIdentifier(identifier);
        m_readOnlyFilesByIdentifier.remove(identifier);
        return {};
    }

    const QString filename = fileInfo.absoluteFilePath();
    const QDateTime lastModified = fileInfo.lastModified();
    const qint64 size = fileInfo.size();

    const auto cachedIt = m_containerCache.constFind(identifier);
    if (cachedIt != m_containerCache.constEnd()) {
        const CachedContainer &cached = cachedIt.value();
        if (cached.container && cached.filename == filename && cached.lastModified == lastModified && cached.size == size) {
            qDebug() << "Container aus Cache:" << identifier;
            return cached.container;
        }

        qDebug() << "Container cache stale:" << identifier << filename << "old mtime:" << cached.lastModified << "new mtime:" << lastModified
                 << "old size:" << cached.size << "new size:" << size;
        m_containerCache.remove(identifier);
        clearServedResourcesForIdentifier(identifier);
    }

    auto container = std::make_shared<EPubContainer>(nullptr);

    if (!container->openFile(filename)) {
        qWarning() << "EPUB konnte nicht geöffnet werden:" << filename;
        return {};
    }

    m_containerCache.insert(identifier, CachedContainer{container, filename, lastModified, size});

    qDebug() << "Container aus DB geladen:" << identifier << filename << "mtime:" << lastModified << "size:" << size;

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

auto serveStaticFile = [](QString filePath) -> QHttpServerResponse {
    if (filePath.isEmpty())
        return {QHttpServerResponder::StatusCode::NotFound};

    const QUrl url(filePath);
    if (url.isLocalFile())
        filePath = url.toLocalFile();

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return {QHttpServerResponder::StatusCode::NotFound};

    const QByteArray data = file.readAll();

    QMimeDatabase mimeDatabase;
    const QByteArray mimeType = mimeDatabase.mimeTypeForFile(filePath, QMimeDatabase::MatchExtension).name().toUtf8();

    QHttpServerResponse response(mimeType.isEmpty() ? QByteArrayLiteral("application/octet-stream") : mimeType, data);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    auto headers = response.headers();
    headers.append("Cache-Control", "no-store");
    response.setHeaders(headers);
#else
    response.setHeader("Cache-Control", "no-store");
#endif
    qDebug() << "Serving static file:" << filePath << "mime type:" << mimeType;
    return response;
};

BookServer::BookServer(const QString &serverToken, bool quitWhenUnused)
    : m_serverToken(serverToken)
    , m_quitWhenUnused(quitWhenUnused)
{
    addSessionToken(m_serverToken);

    server.route(QStringLiteral("/static/background-image"), [](const QHttpServerRequest &request) {
        Q_UNUSED(request)
        return serveStaticFile(Config::readerBackgroundPath());
    });

    server.route(QStringLiteral("/static/book-icon"), [](const QHttpServerRequest &) {
        qDebug() << "Serving book icon";
        return serveStaticFile(QStringLiteral(":/qt/qml/org/kde/arianna/qml/icons/book.svg"));
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
        registerReaderSession(sessionToken);

        QJsonObject obj;
        obj.insert(QStringLiteral("sessionToken"), sessionToken);
        obj.insert(QStringLiteral("headerName"), QStringLiteral("X-Arianna-Session-Token"));
        obj.insert(QStringLiteral("registeredReaders"), registeredReaderCount());

        return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    server.route(QStringLiteral("/session"), QHttpServerRequest::Method::Delete, [this](const QHttpServerRequest &request) {
        const QString sessionToken = requestToken(request);

        if (!isValidToken(sessionToken)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        if (!unregisterReaderSession(sessionToken)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const int remainingReaders = registeredReaderCount();
        const bool serverStopping = remainingReaders == 0;

        QJsonObject obj;
        obj.insert(QStringLiteral("registeredReaders"), remainingReaders);
        obj.insert(QStringLiteral("serverStopping"), serverStopping);

        if (serverStopping) {
            scheduleStopIfUnused();
        }

        return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    server.route(QStringLiteral("/read-only-books"), QHttpServerRequest::Method::Options, [](const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });

    server.route(QStringLiteral("/read-only-books"), QHttpServerRequest::Method::Post, [this](const QHttpServerRequest &request) {
        const QString sessionToken = requestToken(request);

        if (!m_readerSessionRefCount.contains(sessionToken)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        const QJsonDocument document = QJsonDocument::fromJson(request.body());
        const QString fileName = document.object().value(QStringLiteral("fileName")).toString();
        const QFileInfo fileInfo(fileName);
        if (!fileInfo.exists() || !fileInfo.isFile()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }

        const QString identifier = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_readOnlyFilesByIdentifier.insert(identifier, fileInfo.absoluteFilePath());
        registerReaderSessionForIdentifier(sessionToken, identifier);

        QJsonObject obj;
        obj.insert(QStringLiteral("identifier"), identifier);

        qDebug() << "BookServer read-only book registered:" << identifier << fileInfo.absoluteFilePath();

        return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(obj).toJson(QJsonDocument::Compact));
    });

    /* Route for EPUB files */
    server.route(QStringLiteral("/<arg>/book.epub"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/book.epub"), [this](const QString &identifier, const QHttpServerRequest &request) {
        qDebug() << "Request for book URL:" << request.url() << "request token" << requestToken(request);
        const QString token = requestToken(request);
        const QString requestedResourceMode = normalizeResourceMode(
            deliveryQueryValue(request,
                               {QStringLiteral("resourceMode"), QStringLiteral("resourceOutsourcing"), QStringLiteral("resource-outsourcing")},
                               QStringLiteral("auto")));
        const QString referencingMode = normalizeReferencingMode(
            deliveryQueryValue(request,
                               {QStringLiteral("referencingMode"), QStringLiteral("referenceMode"), QStringLiteral("referencing-mode")},
                               QStringLiteral("reader")));

        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        const bool readerSessionToken = m_readerSessionRefCount.contains(token);
        const QString resourceSessionToken = readerSessionToken ? token : QString();
        if (readerSessionToken) {
            registerReaderSessionForIdentifier(token, identifier);
        }

        auto container = containerForIdentifier(identifier);
        if (!container) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
        }
        const bool outsourceResources = requestedResourceMode != QStringLiteral("include");
        const bool includeReferences = referencingMode == QStringLiteral("include");
        const QSet<QString> servedFiles = outsourceResources ? makeServedFiles(container) : QSet<QString>{};
        const QString effectiveResourceMode = servedFiles.isEmpty() ? QStringLiteral("include") : QStringLiteral("outsource");
        if (readerSessionToken) {
            clearServedResourcesForSessionIdentifier(token, identifier);
        } else {
            clearServedResourcesForIdentifier(identifier);
        }
        if (servedFiles.isEmpty() && !includeReferences && requestedResourceMode != QStringLiteral("outsource")) {
            m_containerCache.remove(identifier);
            qDebug() << "Serving original EPUB directly:" << container->filename();
            QHttpServerResponse response = bookFileResponse(container->filename());
            addBookDeliveryHeaders(response, effectiveResourceMode, referencingMode);
            return response;
        }

        m_servedFilesByIdentifier[identifier] = servedFiles;
        const ResourceMap resourceMap = makeResourceMap(servedFiles, identifier, resourceSessionToken);
        ServerReadyEpubOptions deliveryOptions;
        deliveryOptions.resourceMap = resourceMap;
        deliveryOptions.includeReferences = includeReferences;
        if (includeReferences) {
            ReferenceStore referenceStore;
            deliveryOptions.references = epubReferencesFromVariantList(referenceStore.loadReferences(identifier));
        }
        const QByteArray data = container->createServerReadyEpub(deliveryOptions);

        qDebug() << "Served files map for identifier:" << identifier << resourceMap << "referencing mode:" << referencingMode;

        if (data.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::InternalServerError};
        }

        QHttpServerResponse response(QByteArrayLiteral("application/epub+zip"), data);
        addBookResponseHeaders(response, data.size());
        addBookDeliveryHeaders(response, effectiveResourceMode, referencingMode);

        return response;
    });

    server.route(QStringLiteral("/<arg>/refs"), QHttpServerRequest::Method::Options, [](const QString & /*identifier*/, const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });

    server.route(QStringLiteral("/<arg>/refs"), QHttpServerRequest::Method::Get, [this](const QString &identifier, const QHttpServerRequest &request) {
        const QString token = requestToken(request);
        qDebug() << "Processing request to list references for book:" << identifier << "with token:" << token;
        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }

        ReferenceStore referenceStore;
        const QVariantList refs = referenceStore.loadReferences(identifier);

        QJsonObject root;
        root.insert(QStringLiteral("bookId"), identifier);
        root.insert(QStringLiteral("refs"), QJsonArray::fromVariantList(refs));

        QJsonDocument doc(root);
        QHttpServerResponse response("application/json", doc.toJson(QJsonDocument::Compact), QHttpServerResponder::StatusCode::Ok);
        QHttpHeaders headers;
        headers.append("Cache-Control", "no-cache");
        response.setHeaders(headers);
        return response;
    });

    server.route(QStringLiteral("/<arg>/targetLocations"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/targetLocations"),
                 QHttpServerRequest::Method::Get,
                 [this](const QString &identifier, const QHttpServerRequest &request) {
                     const QString token = requestToken(request);
                     qDebug() << "Processing request to list target locations for book:" << identifier << "with token:" << token;
                     if (!isValidToken(token)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
                     }

                     const auto container = containerForIdentifier(identifier);
                     if (!container) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                     }

                     QJsonArray targetLocations;
                     QJsonArray locationStrings;
                     for (const TargetAnchorInfo &target : container->referenceableTargets()) {
                         if (target.ref.isEmpty() || target.location.isEmpty()) {
                             continue;
                         }

                         QJsonObject item;
                         item.insert(QStringLiteral("id"), target.ref);
                         item.insert(QStringLiteral("location"), target.location);
                         item.insert(QStringLiteral("file"), target.file);
                         item.insert(QStringLiteral("previewHtml"), target.previewHtml);
                         item.insert(QStringLiteral("title"), target.title);
                         item.insert(QStringLiteral("type"), target.type);
                         targetLocations.append(item);
                         locationStrings.append(target.location);
                     }

                     QJsonObject root;
                     root.insert(QStringLiteral("bookId"), identifier);
                     root.insert(QStringLiteral("targetLocations"), targetLocations);
                     root.insert(QStringLiteral("locations"), locationStrings);

                     QJsonDocument doc(root);
                     QHttpServerResponse response("application/json", doc.toJson(QJsonDocument::Compact), QHttpServerResponder::StatusCode::Ok);
                     QHttpHeaders headers;
                     headers.append("Cache-Control", "no-cache");
                     response.setHeaders(headers);
                     return response;
                 });

    server.route(QStringLiteral("/<arg>/targetLocations"),
                 QHttpServerRequest::Method::Post,
                 [this](const QString &identifier, const QHttpServerRequest &request) {
                     const QString token = requestToken(request);
                     qDebug() << "Processing request to create target location for book:" << identifier << "with token:" << token;
                     if (!isValidToken(token)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
                     }

                     const QJsonDocument document = QJsonDocument::fromJson(request.body());
                     const QJsonObject obj = document.object();
                     const QString cfi = obj.value(QStringLiteral("targetCFI")).toString(obj.value(QStringLiteral("cfi")).toString()).trimmed();
                     if (cfi.isEmpty()) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
                     }

                     const auto container = containerForIdentifier(identifier);
                     if (!container) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                     }

                     const QString targetAnchorId = container->createAnchor(cfi, QString());
                     if (targetAnchorId.isEmpty()) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
                     }

                     m_containerCache.remove(identifier);
                     clearServedResourcesForIdentifier(identifier);

                     QJsonObject root;
                     root.insert(QStringLiteral("targetAnchorId"), targetAnchorId);
                     root.insert(QStringLiteral("id"), targetAnchorId);

                     const auto anchoredContainer = containerForIdentifier(identifier);
                     if (anchoredContainer) {
                         anchoredContainer->extractTargetAnchors();
                         if (const TargetAnchorInfo *anchor = anchoredContainer->targetAnchorByRef(targetAnchorId)) {
                             root.insert(QStringLiteral("location"), anchor->location);
                             root.insert(QStringLiteral("previewHtml"), anchor->previewHtml);
                             root.insert(QStringLiteral("file"), anchor->file);
                             root.insert(QStringLiteral("title"), anchor->title);
                             root.insert(QStringLiteral("type"), anchor->type);
                         }
                     }

                     return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(root).toJson(QJsonDocument::Compact));
                 });
    // Route for book resources
    server.route(QStringLiteral("/<arg>/res/<arg>"),
                 QHttpServerRequest::Method::Options,
                 [](const QString & /*identifier*/, const QString & /*resourceUuid*/, const QHttpServerRequest &request) {
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/res/<arg>"), [this](const QString &identifier, const QString &resourceUuid, const QHttpServerRequest &request) {
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

        if (!served.sessionToken.isEmpty() && served.sessionToken != token) {
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

    server.route(QStringLiteral("/<arg>/ref/<arg>"),
                 QHttpServerRequest::Method::Options,
                 [](const QString &, const QString &, const QHttpServerRequest &request) {
                     qDebug() << "Processing OPTIONS request for reference from book:";
                     return corsPreflightResponse(request);
                 });

    server.route(QStringLiteral("/<arg>/ref/<arg>"),
                 QHttpServerRequest::Method::Get,
                 [this](const QString &sourceBookId, const QString &sourceAnchorId, const QHttpServerRequest &request) {
                     const QString token = requestToken(request);
                     qDebug() << "Processing request for reference from book:" << sourceBookId << "anchor:" << sourceAnchorId;
                     if (!isValidToken(token)) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
                     }
                     qDebug() << "Request for reference from book:" << sourceBookId << "anchor:" << sourceAnchorId << "with token:" << token;
                     ReferenceStore referenceStore;
                     QVariantMap ref = referenceStore.loadReferenceBySource(sourceBookId, sourceAnchorId);
                     if (ref.isEmpty()) {
                         return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                     }
                     // Resolve target anchor details if we don't already have location or preview metadata.
                     QJsonArray result;
                     QJsonObject obj;
                     const QString targetAnchorId = ref.value(QStringLiteral("targetAnchorId")).toString();
                     const QString targetBookId = ref.value(QStringLiteral("targetBookId")).toString();
                     const QString targetLocation = ref.value(QStringLiteral("targetLocation")).toString();
                     const QString targetPreviewHtml = ref.value(QStringLiteral("targetPreviewHtml")).toString();
                     TargetAnchorInfo resolvedTarget;
                     bool hasResolvedTarget = false;
                     const bool needsAnchorLookup = (targetLocation.isEmpty() || targetPreviewHtml.isEmpty()) && !targetBookId.isEmpty();
                     if (needsAnchorLookup) {
                         const auto targetContainer = containerForIdentifier(targetBookId);
                         if (!targetContainer) {
                             return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                         }
                         const QString lookupValue = !targetAnchorId.isEmpty() ? targetAnchorId : targetLocation;
                         hasResolvedTarget = findReferenceableTarget(targetContainer, lookupValue, &resolvedTarget);
                         if (!hasResolvedTarget) {
                             return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
                         }

                         qDebug() << "Found target in EPUB:" << resolvedTarget.ref << "Location:" << resolvedTarget.location
                                  << "preview:" << resolvedTarget.previewHtml;
                         const QString bookTitle = targetContainer->metadata(QStringLiteral("title")).value(0, QStringLiteral("Unbekannter Titel"));
                         const QString targetLabel = targetAnchorId.isEmpty() ? resolvedTarget.ref : targetAnchorId;
                         const QString shortName = bookTitle + QStringLiteral(" (") + targetLabel + QStringLiteral(")");
                         obj.insert(QStringLiteral("shortName"), shortName);

                         if (targetLocation.isEmpty() || targetPreviewHtml.isEmpty()) {
                             qDebug() << "Target location or preview HTML is empty, updating reference store with target details";
                             ref.insert(QStringLiteral("targetLocation"), resolvedTarget.location);
                             ref.insert(QStringLiteral("targetPreviewHtml"), resolvedTarget.previewHtml);
                             referenceStore.saveReference(ref);
                         }
                     }

                     // obj[QStringLiteral("sourceBookId")] = sourceBookId;
                     // obj[QStringLiteral("targetBookId")] = targetBookId;
                     obj[QStringLiteral("location")] = !targetLocation.isEmpty() ? targetLocation : (hasResolvedTarget ? resolvedTarget.location : QString());
                     obj[QStringLiteral("previewHtml")] =
                         !targetPreviewHtml.isEmpty() ? targetPreviewHtml : (hasResolvedTarget ? resolvedTarget.previewHtml : QString());

                     QJsonObject entryObj;
                     const auto targetEntry = BookDatabase::self().loadEntryByUniqueIdentifier(targetBookId);
                     if (targetEntry) {
                         entryObj.insert(QStringLiteral("filename"), targetEntry->filename);
                         entryObj.insert(QStringLiteral("filetitle"), targetEntry->filetitle);
                         entryObj.insert(QStringLiteral("title"), targetEntry->title);
                         entryObj.insert(QStringLiteral("locations"), targetEntry->locations);
                         entryObj.insert(QStringLiteral("currentLocation"), targetEntry->currentLocation);
                         entryObj.insert(QStringLiteral("uniqueIdentifier"), targetEntry->uniqueIdentifier);
                         entryObj.insert(QStringLiteral("identifier"), targetEntry->identifier);
                         entryObj.insert(QStringLiteral("zoomLevel"), targetEntry->zoomLevel);
                     } else if (m_readOnlyFilesByIdentifier.contains(targetBookId)) {
                         entryObj.insert(QStringLiteral("filename"), m_readOnlyFilesByIdentifier.value(targetBookId));
                         entryObj.insert(QStringLiteral("uniqueIdentifier"), targetBookId);
                     }
                     obj.insert(QStringLiteral("entry"), entryObj);
                     obj.insert(QStringLiteral("readOnly"), m_readOnlyFilesByIdentifier.contains(targetBookId));

                     QJsonObject root;
                     root[QStringLiteral("target")] = obj;

                     QJsonDocument doc(root);
                     QHttpServerResponse response("application/json", doc.toJson(QJsonDocument::Compact), QHttpServerResponder::StatusCode::Ok);
                     QHttpHeaders headers;
                     headers.append("Cache-Control", "no-cache");
                     response.setHeaders(headers);
                     return response;
                 });
    /* route for all books*/
    server.route(QStringLiteral("/books"), QHttpServerRequest::Method::Options, [](const QHttpServerRequest &request) {
        return corsPreflightResponse(request);
    });
    server.route(QStringLiteral("/books"), QHttpServerRequest::Method::Get, [this](const QHttpServerRequest &request) {
        const QString token = requestToken(request);
        qDebug() << "Processing request to list all books with token:" << token;
        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }
        qDebug() << "   Request to list all books with token:" << token;

        QJsonArray books;
        QList<BookEntry> entries = BookDatabase::self().loadEntries();
        std::sort(entries.begin(), entries.end(), [](const BookEntry &left, const BookEntry &right) {
            const QString leftId = left.uniqueIdentifier.isEmpty() ? left.identifier : left.uniqueIdentifier;
            const QString rightId = right.uniqueIdentifier.isEmpty() ? right.identifier : right.uniqueIdentifier;
            const QString leftTitle = left.title.isEmpty() ? leftId : left.title;
            const QString rightTitle = right.title.isEmpty() ? rightId : right.title;
            const int titleCompare = QString::localeAwareCompare(leftTitle, rightTitle);
            if (titleCompare != 0) {
                return titleCompare < 0;
            }

            return QString::localeAwareCompare(leftId, rightId) < 0;
        });
        for (const BookEntry &entry : entries) {
            QJsonObject book;
            const QString id = entry.uniqueIdentifier.isEmpty() ? entry.identifier : entry.uniqueIdentifier;
            book.insert(QStringLiteral("id"), id);
            book.insert(QStringLiteral("title"), entry.title);
            book.insert(QStringLiteral("author"), QJsonArray::fromStringList(entry.author));
            books.append(book);
        }

        QJsonObject root;
        root.insert(QStringLiteral("books"), books);

        QJsonDocument doc(root);
        QHttpServerResponse response("application/json", doc.toJson(QJsonDocument::Compact), QHttpServerResponder::StatusCode::Ok);
        QHttpHeaders headers;
        headers.append("Cache-Control", "no-cache");
        response.setHeaders(headers);
        return response;
    });

    /* route for reference (POST)*/
    server.route(QStringLiteral("/<arg>/ref"), QHttpServerRequest::Method::Options, [](const QString &sourceBookId, const QHttpServerRequest &request) {
        qDebug() << "Processing OPTIONS request to save reference for book:" << sourceBookId;
        return corsPreflightResponse(request);
    });
    server.route(QStringLiteral("/<arg>/ref"), QHttpServerRequest::Method::Post, [this](const QString &sourceBookId, const QHttpServerRequest &request) {
        const QString token = requestToken(request);
        qDebug() << "Processing request to save reference for book:" << sourceBookId;
        if (!isValidToken(token)) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::Unauthorized};
        }
        qDebug() << "Request to save reference from book:" << sourceBookId << "with token:" << token;

        const QJsonDocument document = QJsonDocument::fromJson(request.body());
        qDebug() << "Request body:" << document.toJson(QJsonDocument::Compact);
        const QJsonObject obj = document.object();
        if (obj.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }

        ReferenceStore referenceStore;
        QVariantMap ref;
        QString sourceAnchorId = obj.value(QStringLiteral("sourceAnchorId")).toString().trimmed();
        const QString sourceCfi = obj.value(QStringLiteral("cfi")).toString().trimmed();
        if (sourceCfi.isEmpty() && sourceAnchorId.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }

        const QString targetBookId = obj.value(QStringLiteral("targetBookId")).toString();

        QString targetAnchorId = obj.value(QStringLiteral("targetAnchorId")).toString();
        QString targetLocation = obj.value(QStringLiteral("targetLocation")).toString();
        QString targetPreviewHtml = obj.value(QStringLiteral("targetPreviewHtml")).toString();

        const QString targetCFI = obj.value(QStringLiteral("targetCFI")).toString().trimmed();
        const bool hasSelectedTargetLocation = !targetAnchorId.isEmpty() || !targetLocation.isEmpty();
        if (targetBookId.isEmpty() || (!hasSelectedTargetLocation && targetCFI.isEmpty())) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }
        const bool isCrossReference = targetBookId != sourceBookId;

        if (targetAnchorId.isEmpty() && !targetLocation.isEmpty()) {
            targetAnchorId = targetLocation;
        }

        if (!targetBookId.isEmpty() && !targetCFI.isEmpty() && !hasSelectedTargetLocation) {
            const auto targetContainer = containerForIdentifier(targetBookId);
            if (!targetContainer) {
                return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
            }

            const QString createdTargetAnchorId = targetContainer->createAnchor(targetCFI, QString());
            if (createdTargetAnchorId.isEmpty()) {
                return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
            }

            targetAnchorId = createdTargetAnchorId;
            m_containerCache.remove(targetBookId);
            clearServedResourcesForIdentifier(targetBookId);

            const auto anchoredTargetContainer = containerForIdentifier(targetBookId);
            if (anchoredTargetContainer) {
                anchoredTargetContainer->extractTargetAnchors();
                if (const TargetAnchorInfo *anchor = anchoredTargetContainer->targetAnchorByRef(targetAnchorId)) {
                    targetLocation = anchor->location;
                    targetPreviewHtml = anchor->previewHtml;
                }
            }
        } else if (targetPreviewHtml.isEmpty()) {
            const auto targetContainer = containerForIdentifier(targetBookId);
            TargetAnchorInfo target;
            const QString lookupValue = !targetAnchorId.isEmpty() ? targetAnchorId : targetLocation;
            if (findReferenceableTarget(targetContainer, lookupValue, &target)) {
                if (targetAnchorId.isEmpty()) {
                    targetAnchorId = target.ref;
                }
                if (targetLocation.isEmpty()) {
                    targetLocation = target.location;
                }
                targetPreviewHtml = target.previewHtml;
            }
        }

        if (targetAnchorId.isEmpty() || targetLocation.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }

        if (sourceAnchorId.isEmpty()) {
            const auto sourceContainer = containerForIdentifier(sourceBookId);
            if (!sourceContainer) {
                return QHttpServerResponse{QHttpServerResponder::StatusCode::NotFound};
            }

            sourceAnchorId = sourceContainer->createBookrefAnchor(sourceCfi, targetLocation, isCrossReference);
            if (sourceAnchorId.isEmpty()) {
                sourceAnchorId = QStringLiteral("uuid_") + QUuid::createUuidV7().toString(QUuid::WithoutBraces);

                ref.insert(QStringLiteral("sourceBookId"), sourceBookId);
                ref.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
                ref.insert(QStringLiteral("targetBookId"), targetBookId);
                ref.insert(QStringLiteral("targetAnchorId"), targetAnchorId);
                ref.insert(QStringLiteral("targetLocation"), targetLocation);
                ref.insert(QStringLiteral("targetPreviewHtml"), targetPreviewHtml);
                referenceStore.saveReference(ref);

                const QString selectedText = obj.value(QStringLiteral("text")).toString();
                QJsonObject root;
                root.insert(QStringLiteral("error"), QStringLiteral("manualAnchorRequired"));
                root.insert(QStringLiteral("reason"), QStringLiteral("anchorWrapFailed"));
                root.insert(QStringLiteral("bookId"), sourceBookId);
                root.insert(QStringLiteral("cfi"), sourceCfi);
                root.insert(QStringLiteral("text"), selectedText);
                root.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
                root.insert(QStringLiteral("referenceStored"), true);
                root.insert(QStringLiteral("isCrossReference"), isCrossReference);
                root.insert(QStringLiteral("anchorOpenTag"), manualBookrefAnchorOpenTag(sourceAnchorId, targetLocation, isCrossReference));
                root.insert(QStringLiteral("anchorCloseTag"), QStringLiteral("</a>"));
                return QHttpServerResponse(QByteArrayLiteral("application/json"),
                                           QJsonDocument(root).toJson(QJsonDocument::Compact),
                                           QHttpServerResponder::StatusCode::Conflict);
            }

            m_containerCache.remove(sourceBookId);
            clearServedResourcesForIdentifier(sourceBookId);
        } else {
            qDebug() << "Saving reference for existing source anchor:" << sourceAnchorId << "in book:" << sourceBookId;
        }

        if (sourceAnchorId.isEmpty()) {
            return QHttpServerResponse{QHttpServerResponder::StatusCode::BadRequest};
        }

        ref.clear();
        ref.insert(QStringLiteral("sourceBookId"), sourceBookId);
        ref.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
        ref.insert(QStringLiteral("targetBookId"), targetBookId);
        ref.insert(QStringLiteral("targetAnchorId"), targetAnchorId);
        ref.insert(QStringLiteral("targetLocation"), targetLocation);
        ref.insert(QStringLiteral("targetPreviewHtml"), targetPreviewHtml);
        referenceStore.saveReference(ref);

        QJsonObject root;
        root.insert(QStringLiteral("sourceAnchorId"), sourceAnchorId);
        root.insert(QStringLiteral("targetAnchorId"), targetAnchorId);
        root.insert(QStringLiteral("targetLocation"), targetLocation);
        root.insert(QStringLiteral("targetPreviewHtml"), targetPreviewHtml);
        root.insert(QStringLiteral("isCrossReference"), isCrossReference);
        return QHttpServerResponse(QByteArrayLiteral("application/json"), QJsonDocument(root).toJson(QJsonDocument::Compact));
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    server.addAfterRequestHandler(&server, [](const QHttpServerRequest &request, QHttpServerResponse &resp) {
        addCorsHeaders(request, resp);
    });
#else
    server.afterRequest([](QHttpServerResponse &&resp) {
        resp.setHeader("Access-Control-Allow-Origin", "*");
        resp.setHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
        resp.setHeader("Access-Control-Allow-Headers", "X-Arianna-Session-Token, X-Arianna-Server-Token, Range, Content-Type");
        resp.setHeader("Access-Control-Expose-Headers", "Accept-Ranges, Content-Length, Content-Range, X-Arianna-Resource-Mode, X-Arianna-Referencing-Mode");
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

void BookServer::stop()
{
    if (!m_running) {
        return;
    }

    const auto tcpServers = server.servers();
    for (QTcpServer *tcpServer : tcpServers) {
        if (tcpServer) {
            tcpServer->close();
        }
    }

    m_validTokens.clear();
    m_readerSessionRefCount.clear();
    m_identifiersByReaderSession.clear();
    m_readerSessionsByIdentifier.clear();
    m_resourceByUuid.clear();
    m_servedFilesByIdentifier.clear();
    m_resourceMapByIdentifier.clear();
    m_containerCache.clear();
    m_readOnlyFilesByIdentifier.clear();
    m_running = false;

    removeDiscoveryFile();

    qWarning() << "BookServer stopped";
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
    releaseReaderSessionResources(token);
    m_readerSessionRefCount.remove(token);
    m_validTokens.remove(token);

    qDebug() << "BookServer token removed:" << token;
}

void BookServer::registerReaderSession(const QString &token)
{
    if (token.isEmpty()) {
        return;
    }

    addSessionToken(token);
    m_readerSessionRefCount[token] = m_readerSessionRefCount.value(token) + 1;

    qDebug() << "BookServer reader registered:" << token << "registered readers:" << registeredReaderCount();
}

bool BookServer::unregisterReaderSession(const QString &token)
{
    auto it = m_readerSessionRefCount.find(token);
    if (it == m_readerSessionRefCount.end()) {
        qDebug() << "BookServer reader session not registered:" << token;
        return false;
    }

    --it.value();
    if (it.value() <= 0) {
        m_readerSessionRefCount.erase(it);
        m_validTokens.remove(token);
        releaseReaderSessionResources(token);
    }

    qDebug() << "BookServer reader unregistered:" << token << "registered readers:" << registeredReaderCount();
    return true;
}

bool BookServer::hasRegisteredReaders() const
{
    return registeredReaderCount() > 0;
}

int BookServer::registeredReaderCount() const
{
    int count = 0;
    for (auto it = m_readerSessionRefCount.constBegin(); it != m_readerSessionRefCount.constEnd(); ++it) {
        count += it.value();
    }

    return count;
}

void BookServer::scheduleStopIfUnused()
{
    if (hasRegisteredReaders() || m_stopScheduled) {
        return;
    }

    auto *app = QCoreApplication::instance();
    if (!app) {
        stop();
        return;
    }

    m_stopScheduled = true;
    QTimer::singleShot(100, app, [this] {
        m_stopScheduled = false;
        if (hasRegisteredReaders()) {
            return;
        }

        stop();

        if (m_quitWhenUnused) {
            QCoreApplication::quit();
        }
    });
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

ResourceMap BookServer::makeResourceMap(const QSet<QString> &servedFiles, const QString &identifier, const QString &sessionToken)
{
    ResourceMap map;

    for (const QString &path : servedFiles) {
        const QString cleanPath = QDir::cleanPath(path);

        const QString resourceUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

        m_resourceByUuid.insert(resourceUuid, ServedResource{identifier, cleanPath, sessionToken});

        map.insert(cleanPath, makeResourceUrl(identifier, resourceUuid));
    }

    return map;
}

BookServer::~BookServer()
{
    stop();
}
