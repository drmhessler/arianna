// SPDX-FileCopyrightText: 2018 Martin T. H. Sandsmark <martin.sandsmark@kde.org>
// SPDX-FileCopyrightText: 2021 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: BSD-3-Clause

#include "epubcontainer.h"

#include <KArchiveDirectory>
#include <KArchiveFile>
#include <KLocalizedString>

#include <QDebug>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QRegularExpression>
#include <QScopedPointer>

#include <KZip>
#include <QBuffer>
#include <QMimeDatabase>

#define METADATA_FOLDER QStringLiteral("META-INF")
#define MIMETYPE_FILE QStringLiteral("mimetype")
#define CONTAINER_FILE QStringLiteral("META-INF/container.xml")

static QString escapeXmlAttribute(const QString &value)
{
    QString escaped = value;
    escaped.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("&quot;"));
    escaped.replace(QStringLiteral("<"), QStringLiteral("&lt;"));
    escaped.replace(QStringLiteral(">"), QStringLiteral("&gt;"));
    return escaped;
}
static QString appendProperty(const QString &properties, const QString &property)
{
    QStringList parts = properties.split(u' ', Qt::SkipEmptyParts);

    if (!parts.contains(property)) {
        parts.append(property);
    }

    return parts.join(u' ');
}

static bool isXmlLikeFile(const QString &path)
{
    const QString lower = path.toLower();

    return lower.endsWith(QStringLiteral(".xhtml")) || lower.endsWith(QStringLiteral(".html")) || lower.endsWith(QStringLiteral(".htm"))
        || lower.endsWith(QStringLiteral(".svg")) || lower.endsWith(QStringLiteral(".smil"));
}

static bool isRemoteResourceHostMime(const QString &mediaType)
{
    return mediaType == QStringLiteral("application/xhtml+xml") || mediaType == QStringLiteral("image/svg+xml")
        || mediaType == QStringLiteral("application/smil+xml");
}

static bool isCssFile(const QString &path)
{
    return path.toLower().endsWith(QStringLiteral(".css"));
}

static QString resolveRelativePath(const QString &basePath, const QString &relativePath)
{
    if (relativePath.startsWith(QStringLiteral("http://")) || relativePath.startsWith(QStringLiteral("https://"))
        || relativePath.startsWith(QStringLiteral("data:")) || relativePath.startsWith(QStringLiteral("blob:"))
        || relativePath.startsWith(QStringLiteral("#"))) {
        return relativePath;
    }

    const QString cleanRelative = relativePath.section(u'#', 0, 0);
    const QString baseDir = QFileInfo(basePath).path();

    return QDir::cleanPath(baseDir + QStringLiteral("/") + cleanRelative);
}

static QByteArray rewriteCssResourceLinks(const QByteArray &data, const QString &currentPath, const ResourceMap &resourceMap)
{
    QString text = QString::fromUtf8(data);

    static const QRegularExpression urlRegex(QStringLiteral(R"###(url\(\s*['"]?([^'")]+)['"]?\s*\))###"));

    QRegularExpressionMatchIterator it = urlRegex.globalMatch(text);

    QList<QPair<QString, QString>> replacements;

    while (it.hasNext()) {
        const auto match = it.next();

        const QString original = match.captured(0);

        const QString value = match.captured(1);

        if (value.startsWith(QStringLiteral("http://")) || value.startsWith(QStringLiteral("https://")) || value.startsWith(QStringLiteral("data:"))
            || value.startsWith(QStringLiteral("blob:")) || value.startsWith(QStringLiteral("#"))) {
            continue;
        }

        const QString resolved = resolveRelativePath(currentPath, value);

        if (!resourceMap.contains(resolved)) {
            continue;
        }

        const QString rewritten = QStringLiteral("url(\"") + resourceMap.value(resolved) + QStringLiteral("\")");

        replacements.append({original, rewritten});

        qDebug() << "Rewriting CSS resource:" << value << "->" << resourceMap.value(resolved);
    }

    for (const auto &[from, to] : replacements) {
        text.replace(from, to);
    }

    return text.toUtf8();
}

static QByteArray rewriteXmlResourceLinks(const QByteArray &data, const QString &currentPath, const ResourceMap &resourceMap)
{
    QString text = QString::fromUtf8(data);

    static const QRegularExpression attrRegex(QStringLiteral(R"###((src|href|poster)=["']([^"']+)["'])###"));

    QRegularExpressionMatchIterator it = attrRegex.globalMatch(text);

    QList<QPair<QString, QString>> replacements;

    while (it.hasNext()) {
        const auto match = it.next();

        const QString original = match.captured(0);

        const QString attr = match.captured(1);

        const QString value = match.captured(2);

        if (value.startsWith(QStringLiteral("http://")) || value.startsWith(QStringLiteral("https://")) || value.startsWith(QStringLiteral("data:"))
            || value.startsWith(QStringLiteral("blob:")) || value.startsWith(QStringLiteral("#"))) {
            continue;
        }

        const QString resolved = resolveRelativePath(currentPath, value);

        if (!resourceMap.contains(resolved)) {
            continue;
        }

        const QString rewritten = attr + QStringLiteral("=\"") + escapeXmlAttribute(resourceMap.value(resolved)) + QStringLiteral("\"");

        replacements.append({original, rewritten});

        qDebug() << "Rewriting XML resource:" << value << "->" << resourceMap.value(resolved);
    }

    for (const auto &[from, to] : replacements) {
        text.replace(from, to);
    }

    return text.toUtf8();
}

static QByteArray rewriteOpfManifestLinks(const QByteArray &data, const QString &opfPath, const ResourceMap &resourceMap)
{
    QDomDocument doc;

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    if (!doc.setContent(data, QDomDocument::ParseOption::UseNamespaceProcessing)) {
        return data;
    }
#else
    if (!doc.setContent(data, true)) {
        return data;
    }
#endif

    QDomNodeList manifestNodes = doc.elementsByTagName(QStringLiteral("manifest"));
    if (manifestNodes.isEmpty()) {
        return data;
    }

    QDomElement manifest = manifestNodes.at(0).toElement();
    QDomNodeList itemNodes = manifest.elementsByTagName(QStringLiteral("item"));

    for (int i = 0; i < itemNodes.count(); ++i) {
        QDomElement item = itemNodes.at(i).toElement();

        const QString href = item.attribute(QStringLiteral("href"));
        const QString mediaType = item.attribute(QStringLiteral("media-type"));

        if (href.isEmpty()) {
            continue;
        }

        const QString resolved = QDir::cleanPath(resolveRelativePath(opfPath, href));

        // 1. Manifest-Ressource selbst auf Server-URL umbiegen
        if (resourceMap.contains(resolved)) {
            item.setAttribute(QStringLiteral("href"), resourceMap.value(resolved));

            qDebug() << "Rewriting OPF manifest resource:" << resolved << "->" << resourceMap.value(resolved);
        }

        // 2. Content-Dokumente als Host von Remote Resources markieren
        if (isRemoteResourceHostMime(mediaType)) {
            const QString oldProperties = item.attribute(QStringLiteral("properties"));

            item.setAttribute(QStringLiteral("properties"), appendProperty(oldProperties, QStringLiteral("remote-resources")));
        }
    }

    return doc.toByteArray();
}

static void copyDirectoryServerReady(KZip &outZip, const KArchiveDirectory *dir, const QString &prefix, const ResourceMap &resourceMap)
{
    if (!dir) {
        return;
    }

    const QStringList entries = dir->entries();

    for (const QString &entryName : entries) {
        const KArchiveEntry *entry = dir->entry(entryName);
        if (!entry) {
            continue;
        }

        const QString fullPath = prefix.isEmpty() ? entryName : prefix + QStringLiteral("/") + entryName;

        const QString cleanPath = QDir::cleanPath(fullPath);

        if (cleanPath == QStringLiteral("mimetype")) {
            continue;
        }

        if (entry->isDirectory()) {
            const auto *subdir = dynamic_cast<const KArchiveDirectory *>(entry);

            if (subdir) {
                copyDirectoryServerReady(outZip, subdir, cleanPath, resourceMap);
            }

            continue;
        }

        const auto *file = dynamic_cast<const KArchiveFile *>(entry);

        if (!file) {
            continue;
        }

        // Diese Dateien werden jetzt vom BookServer geliefert.
        if (resourceMap.contains(cleanPath)) {
            continue;
        }

        QScopedPointer<QIODevice> dev(file->createDevice());
        if (!dev) {
            continue;
        }

        QByteArray data = dev->readAll();

        const QString lower = cleanPath.toLower();

        if (lower.endsWith(QStringLiteral(".opf"))) {
            data = rewriteOpfManifestLinks(data, cleanPath, resourceMap);
        } else if (isXmlLikeFile(cleanPath)) {
            data = rewriteXmlResourceLinks(data, cleanPath, resourceMap);
        } else if (isCssFile(cleanPath)) {
            data = rewriteCssResourceLinks(data, cleanPath, resourceMap);
        }

        outZip.writeFile(cleanPath, data);
    }
}

EPubContainer::EPubContainer(QObject *parent)
    : QObject(parent)
    , m_rootFolder(nullptr)
{
}

EPubContainer::~EPubContainer() = default;

bool EPubContainer::openFile(const QString &path)
{
    m_filename = path;
    m_archive = std::make_unique<KZip>(path);

    if (!m_archive->open(QIODevice::ReadOnly)) {
        Q_EMIT errorOccured(i18n("Failed to open %1", path));

        return false;
    }

    m_rootFolder = m_archive->directory();
    if (!m_rootFolder) {
        Q_EMIT errorOccured(i18n("Failed to read %1", path));
        return false;
    }

    if (!parseMimetype()) {
        return false;
    }

    if (!parseContainer()) {
        return false;
    }

    return true;
}

const KArchiveDirectory *EPubContainer::rootDirectory() const
{
    return m_rootFolder;
}

const QHash<QString, EpubItem> &EPubContainer::manifestItems() const
{
    return m_items;
}

QByteArray EPubContainer::createServerReadyEpub(const ResourceMap &resourceMap) const
{
    if (!m_rootFolder) {
        qWarning() << "No EPUB root folder available";
        return {};
    }

    QByteArray result;
    QBuffer buffer(&result);

    if (!buffer.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to open output buffer";
        return {};
    }

    KZip outZip(&buffer);

    if (!outZip.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to create server-ready EPUB";
        return {};
    }

    outZip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));

    copyDirectoryServerReady(outZip, m_rootFolder, QString(), resourceMap);

    outZip.close();
    buffer.close();

    qDebug() << "Created server-ready EPUB with" << resourceMap.size() << "externalized resources";

    return result;
}

QSharedPointer<QIODevice> EPubContainer::ioDevice(const QString &path)
{
    const KArchiveFile *archive = file(path);
    if (!archive) {
        qWarning() << QStringLiteral("Unable to open file %1").arg(path.left(100));
        Q_EMIT errorOccured(i18n("Unable to open file %1", path.left(100)));
        return QSharedPointer<QIODevice>();
    }

    return QSharedPointer<QIODevice>(archive->createDevice());
}

QImage EPubContainer::image(const QString &id)
{
    if (!m_items.contains(id)) {
        qWarning() << "Asked for unknown item" << id << m_items.keys();
        return {};
    }

    const EpubItem &item = m_items.value(id);

    if (!QImageReader::supportedMimeTypes().contains(item.mimetype)) {
        qWarning() << "Asked for unsupported type" << item.mimetype;
        return {};
    }

    QSharedPointer<QIODevice> device = ioDevice(item.path);

    if (!device) {
        return {};
    }

    return QImage::fromData(device->readAll());
}

QByteArray EPubContainer::readData(const QString &path)
{
    auto device = ioDevice(path);
    if (!device) {
        return {};
    }
    return device->readAll();
}

QStringList EPubContainer::metadata(const QStringView &key)
{
    const QString keyString = key.toString();
    if (keyString == QStringLiteral("identifier")) {
        return m_metadata.value(QStringLiteral("identifiers"));
    }
    return m_metadata.value(keyString);
}

bool EPubContainer::parseMimetype()
{
    Q_ASSERT(m_rootFolder);

    const KArchiveFile *mimetypeFile = m_rootFolder->file(MIMETYPE_FILE);

    if (!mimetypeFile) {
        Q_EMIT errorOccured(i18n("Unable to find mimetype in file"));
        return false;
    }

    QScopedPointer<QIODevice> ioDevice(mimetypeFile->createDevice());
    QByteArray mimetype = ioDevice->readAll();
    if (mimetype != "application/epub+zip") {
        qWarning() << "Unexpected mimetype" << mimetype;
    }

    return true;
}

bool EPubContainer::parseContainer()
{
    Q_ASSERT(m_rootFolder);

    const KArchiveFile *containerFile = file(CONTAINER_FILE);
    if (!containerFile) {
        qWarning() << "no container file";
        Q_EMIT errorOccured(i18n("Unable to find container information"));
        return false;
    }

    QScopedPointer<QIODevice> ioDevice(containerFile->createDevice());
    Q_ASSERT(ioDevice);

    // The only thing we need from this file is the path to the root file
    QDomDocument document;
    document.setContent(ioDevice.data());
    QDomNodeList rootNodes = document.elementsByTagName(QStringLiteral("rootfile"));
    for (int i = 0; i < rootNodes.count(); i++) {
        QDomElement rootElement = rootNodes.at(i).toElement();
        QString rootfilePath = rootElement.attribute(QStringLiteral("full-path"));
        if (rootfilePath.isEmpty()) {
            qWarning() << "Invalid root file entry";
            continue;
        }
        if (parseContentFile(rootfilePath)) {
            return true;
        }
    }

    // Limitations:
    //  - We only read one rootfile
    //  - We don't read the following from META-INF/
    //     - manifest.xml (unknown contents, just reserved)
    //     - metadata.xml (unused according to spec, just reserved)
    //     - rights.xml (reserved for DRM, not standardized)
    //     - signatures.xml (signatures for files, standardized)

    Q_EMIT errorOccured(i18n("Unable to find and use any content files"));
    return false;
}

bool EPubContainer::parseContentFile(const QString &filepath)
{
    const KArchiveFile *rootFile = file(filepath);
    if (!rootFile) {
        Q_EMIT errorOccured(i18n("Malformed metadata, unable to get content metadata path"));
        return false;
    }
    QScopedPointer<QIODevice> ioDevice(rootFile->createDevice());
    QDomDocument document;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    document.setContent(ioDevice.data(), QDomDocument::ParseOption::UseNamespaceProcessing); // turn on namespace processing
#else
    document.setContent(ioDevice.data(), true); // turn on namespace processing
#endif

    const QString uniqueIdentifierId = document.documentElement().attribute(QStringLiteral("unique-identifier"));
    QString uniqueIdentifier;

    QDomNodeList metadataNodeList = document.elementsByTagName(QStringLiteral("metadata"));
    for (int i = 0; i < metadataNodeList.count(); i++) {
        QDomNodeList metadataChildList = metadataNodeList.at(i).childNodes();
        for (int j = 0; j < metadataChildList.count(); j++) {
            const QDomNode metadataNode = metadataChildList.at(j);
            const QDomElement metadataElement = metadataNode.toElement();
            if (!uniqueIdentifierId.isEmpty() && metadataElement.prefix() == QStringLiteral("dc") && metadataElement.tagName() == QStringLiteral("identifier")
                && metadataElement.attribute(QStringLiteral("id")) == uniqueIdentifierId) {
                uniqueIdentifier = metadataElement.text().trimmed();
            }
            parseMetadataItem(metadataNode, metadataChildList);
        }
    }

    if (uniqueIdentifier.isEmpty()) {
        uniqueIdentifier = m_metadata.value(QStringLiteral("identifiers")).value(0);
    }
    if (!uniqueIdentifier.isEmpty()) {
        m_metadata[QStringLiteral("unique-identifier")] = QStringList{uniqueIdentifier};
    }

    // Extract current path, for resolving relative paths
    QString contentFileFolder;
    int separatorIndex = filepath.lastIndexOf(QLatin1Char('/'));
    if (separatorIndex > 0) {
        contentFileFolder = filepath.left(separatorIndex + 1);
    }

    // Parse out all the components/items in the epub
    QDomNodeList manifestNodeList = document.elementsByTagName(QStringLiteral("manifest"));
    for (int i = 0; i < manifestNodeList.count(); i++) {
        QDomElement manifestElement = manifestNodeList.at(i).toElement();
        QDomNodeList manifestItemList = manifestElement.elementsByTagName(QStringLiteral("item"));

        for (int j = 0; j < manifestItemList.count(); j++) {
            parseManifestItem(manifestItemList.at(j), contentFileFolder);
        }
    }

    // Parse out the document order
    QDomNodeList spineNodeList = document.elementsByTagName(QStringLiteral("spine"));
    for (int i = 0; i < spineNodeList.count(); i++) {
        QDomElement spineElement = spineNodeList.at(i).toElement();

        QString tocId = spineElement.attribute(QStringLiteral("toc"));
        if (!tocId.isEmpty() && m_items.contains(tocId)) {
            EpubPageReference tocReference;
            tocReference.title = i18n("Table of Contents");
            tocReference.target = tocId;
            m_standardReferences.insert(EpubPageReference::TableOfContents, tocReference);
        }

        QDomNodeList spineItemList = spineElement.elementsByTagName(QStringLiteral("itemref"));
        for (int j = 0; j < spineItemList.count(); j++) {
            parseSpineItem(spineItemList.at(j));
        }
    }

    // Parse out standard items
    QDomNodeList guideNodeList = document.elementsByTagName(QStringLiteral("guide"));
    for (int i = 0; i < guideNodeList.count(); i++) {
        QDomElement guideElement = guideNodeList.at(i).toElement();

        QDomNodeList guideItemList = guideElement.elementsByTagName(QStringLiteral("reference"));
        for (int j = 0; j < guideItemList.count(); j++) {
            parseGuideItem(guideItemList.at(j));
        }
    }

    return true;
}

bool EPubContainer::parseMetadataPropertyItem(const QDomElement &metadataElement, const QDomNodeList &nodeList)
{
    if (metadataElement.attribute(QStringLiteral("property")) == QStringLiteral("belongs-to-collection")) {
        const QString id = QStringLiteral("#") + metadataElement.attribute(QStringLiteral("id"));
        const QString name = metadataElement.text();
        Collection::Type type = Collection::Type::Unknow;
        size_t position = 0;

        if (id.length() == 1) {
            m_collections.append(Collection{name, type, position});
            return true;
        }

        for (int i = 0; i < nodeList.size(); i++) {
            const auto node = nodeList.at(i);
            const auto element = node.toElement();
            if (element.tagName() != QStringLiteral("meta")) {
                continue;
            }

            if (element.attribute(QStringLiteral("refines")) != id) {
                continue;
            }

            if (element.attribute(QStringLiteral("property")) == QStringLiteral("collection-type")) {
                const auto typeString = element.text();
                if (typeString == QStringLiteral("set")) {
                    type = Collection::Type::Set;
                } else if (typeString == QStringLiteral("series")) {
                    type = Collection::Type::Series;
                }
                continue;
            }

            if (element.attribute(QStringLiteral("property")) == QStringLiteral("group-position")) {
                position = element.text().toInt();
                continue;
            }
        }

        m_collections.append(Collection{name, type, position});
        return true;
    }

    return false;
}

bool EPubContainer::parseMetadataItem(const QDomNode &metadataNode, const QDomNodeList &nodeList)
{
    QDomElement metadataElement = metadataNode.toElement();
    QString tagName = metadataElement.tagName();

    QString metaName;
    QString metaValue;

    if (tagName == QStringLiteral("meta")) {
        bool foundProperty = parseMetadataPropertyItem(metadataElement, nodeList);
        if (foundProperty) {
            return true;
        }
        metaName = metadataElement.attribute(QStringLiteral("name"));
        metaValue = metadataElement.attribute(QStringLiteral("content"));
    } else if (metadataElement.prefix() != QStringLiteral("dc")) {
        qWarning() << "Unsupported metadata tag" << tagName;
        return false;
    } else if (tagName == QStringLiteral("date")) {
        metaName = metadataElement.attribute(QStringLiteral("event"));
        metaValue = metadataElement.text();
    } else {
        metaName = tagName;
        metaValue = metadataElement.text();
    }

    if (metaName == QStringLiteral("identifier")) {
        metaName = QStringLiteral("identifiers");
        metaValue = metaValue.trimmed();
    }

    if (metaName.isEmpty() || metaValue.isEmpty()) {
        return false;
    }
    if (!m_metadata.contains(metaName)) {
        m_metadata[metaName] = QStringList{};
    }

    if (metaName != QStringLiteral("subject")) {
        m_metadata[metaName].append(metaValue);
        return true;
    }

    if (metaValue.contains(QStringLiteral("--"))) {
        const auto metaValues = metaValue.split(QStringLiteral("--"));
        if (metaValues.count() <= 1) {
            return false;
        }

        metaValue = metaValues[metaValues.count() - 1].trimmed();
    }

    if (!m_metadata[metaName].contains(metaValue)) {
        m_metadata[metaName].append(metaValue);
        return true;
    }

    return false;
}

bool EPubContainer::parseManifestItem(const QDomNode &manifestNode, const QString &currentFolder)
{
    QDomElement manifestElement = manifestNode.toElement();
    QString id = manifestElement.attribute(QStringLiteral("id"));
    QString path = manifestElement.attribute(QStringLiteral("href"));
    QString type = manifestElement.attribute(QStringLiteral("media-type"));
    const QStringList properties = manifestElement.attribute(QStringLiteral("properties")).split(u' ', Qt::SkipEmptyParts);

    if (id.isEmpty() || path.isEmpty()) {
        qWarning() << "Invalid item at line" << manifestElement.lineNumber();
        return false;
    }

    // Resolve relative paths
    path = QDir::cleanPath(currentFolder + path);

    EpubItem item;
    item.mimetype = type.toUtf8();
    item.path = path;
    m_items[id] = item;

    if (properties.contains(QStringLiteral("cover-image")) && m_metadata.value(QStringLiteral("cover")).isEmpty()) {
        m_metadata[QStringLiteral("cover")] = QStringList{id};
    }

    static QSet<QString> documentTypes(
        {QStringLiteral("text/x-oeb1-document"), QStringLiteral("application/x-dtbook+xml"), QStringLiteral("application/xhtml+xml")});
    // All items not listed in the spine should be in this
    if (documentTypes.contains(type)) {
        m_unorderedItems.insert(id);
    }

    return true;
}

bool EPubContainer::parseSpineItem(const QDomNode &spineNode)
{
    QDomElement spineElement = spineNode.toElement();

    // Ignore this for now
    if (spineElement.attribute(QStringLiteral("linear")) == QStringLiteral("no")) {
        //        return true;
    }

    QString referenceName = spineElement.attribute(QStringLiteral("idref"));
    if (referenceName.isEmpty()) {
        qWarning() << "Invalid spine item at line" << spineNode.lineNumber();
        return false;
    }

    if (!m_items.contains(referenceName)) {
        qWarning() << "Unable to find" << referenceName << "in items";
        return false;
    }

    m_unorderedItems.remove(referenceName);
    m_orderedItems.append(referenceName);

    return true;
}

bool EPubContainer::parseGuideItem(const QDomNode &guideItem)
{
    QDomElement guideElement = guideItem.toElement();
    QString target = guideElement.attribute(QStringLiteral("href"));
    QString title = guideElement.attribute(QStringLiteral("title"));
    QString type = guideElement.attribute(QStringLiteral("type"));

    if (target.isEmpty() || title.isEmpty() || type.isEmpty()) {
        qWarning() << "Invalid guide item" << target << title << type;
        return false;
    }

    EpubPageReference reference;
    reference.target = target;
    reference.title = title;

    EpubPageReference::StandardType standardType = EpubPageReference::typeFromString(type);
    if (standardType == EpubPageReference::Other) {
        m_otherReferences[type] = reference;
    } else {
        m_standardReferences[standardType] = reference;
    }

    return true;
}

const KArchiveFile *EPubContainer::file(const QString &path)
{
    if (path.isEmpty()) {
        return nullptr;
    }

    const KArchiveDirectory *folder = m_rootFolder;

    // Try to walk down the correct path
    QStringList pathParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (int i = 0; i < pathParts.count() - 1; i++) {
        QString folderName = pathParts[i];
        const KArchiveEntry *entry = folder->entry(folderName);
        if (!entry) {
            qWarning() << "Unable to find folder name" << folderName << "in" << path.left(100);
            const QStringList entries = folder->entries();
            for (const QString &folderEntry : entries) {
                if (folderEntry.compare(folderName, Qt::CaseInsensitive) == 0) {
                    entry = folder->entry(folderEntry);
                    break;
                }
            }

            if (!entry) {
                qWarning() << "Didn't even find with case-insensitive matching";
                return nullptr;
            }
        }

        if (!entry->isDirectory()) {
            qWarning() << "Expected" << folderName << "to be a directory in path" << path;
            return nullptr;
        }

        folder = dynamic_cast<const KArchiveDirectory *>(entry);
        Q_ASSERT(folder);
    }

    QString filename;
    if (pathParts.isEmpty()) {
        filename = path;
    } else {
        filename = pathParts.last();
    }

    const KArchiveFile *file = folder->file(filename);
    if (!file) {
        qWarning() << "Unable to find file" << filename << "in" << folder->name();

        const QStringList entries = folder->entries();
        for (const QString &folderEntry : entries) {
            if (folderEntry.compare(filename, Qt::CaseInsensitive) == 0) {
                file = folder->file(folderEntry);
                break;
            }
        }

        if (!file) {
            qWarning() << "Unable to find file" << filename << "in" << folder->name() << "with case-insensitive matching" << entries;
        }
    }
    return file;
}

EpubPageReference::StandardType EpubPageReference::typeFromString(const QString &name)
{
    if (name == QStringLiteral("cover")) {
        return CoverPage;
    } else if (name == QStringLiteral("title-page")) {
        return TitlePage;
    } else if (name == QStringLiteral("toc")) {
        return TableOfContents;
    } else if (name == QStringLiteral("index")) {
        return Index;
    } else if (name == QStringLiteral("glossary")) {
        return Glossary;
    } else if (name == QStringLiteral("acknowledgements")) {
        return Acknowledgements;
    } else if (name == QStringLiteral("bibliography")) {
        return Bibliography;
    } else if (name == QStringLiteral("colophon")) {
        return Colophon;
    } else if (name == QStringLiteral("copyright-page")) {
        return CopyrightPage;
    } else if (name == QStringLiteral("dedication")) {
        return Dedication;
    } else if (name == QStringLiteral("epigraph")) {
        return Epigraph;
    } else if (name == QStringLiteral("foreword")) {
        return Foreword;
    } else if (name == QStringLiteral("loi")) {
        return ListOfIllustrations;
    } else if (name == QStringLiteral("lot")) {
        return ListOfTables;
    } else if (name == QStringLiteral("notes")) {
        return Notes;
    } else if (name == QStringLiteral("preface")) {
        return Preface;
    } else if (name == QStringLiteral("text")) {
        return Text;
    } else {
        return Other;
    }
}

QList<Collection> EPubContainer::collections() const
{
    return m_collections;
}

#include "moc_epubcontainer.cpp"
