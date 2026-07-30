// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "booktruthstore.h"

#include "bookdatabase.h"
#include "categoryentriesmodel.h"
#include "epubcontainer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryFile>

#include <algorithm>

namespace
{
struct BookHashes {
    bool success = false;
    QString errorMessage;
    QByteArray textContentHash;
    QByteArray documentStateHash;
};

struct FileReplacementBackup {
    QString targetPath;
    QString backupPath;
    bool hadOriginal = false;
};

static QString anchoredEpubPath(const QString &filename, const QString &bookId)
{
    QFileInfo fileInfo(filename);
    QString fileStem = bookId.trimmed();
    if (fileStem.isEmpty()) {
        fileStem = fileInfo.completeBaseName();
    }

    fileStem.replace(QRegularExpression(QStringLiteral("[/\\\\]")), QStringLiteral("_"));
    return QFileInfo(fileInfo.dir().filePath(fileStem + QStringLiteral(".anchored.epub"))).absoluteFilePath();
}

static QString authoritativeEpubPath(const QString &filename, const QString &bookId)
{
    const QString anchoredPath = anchoredEpubPath(filename, bookId);
    if (QFileInfo::exists(anchoredPath)) {
        return anchoredPath;
    }

    return QFileInfo(filename).absoluteFilePath();
}

static QString uuidToDatabaseString(const QUuid &uuid)
{
    return uuid.toString(QUuid::WithoutBraces);
}

static QString anchorElementId(const QUuid &uuid)
{
    return QStringLiteral("uuid_") + uuidToDatabaseString(uuid);
}

static QString writeTemporaryEpub(const QString &targetPath, const QByteArray &data, QString *errorMessage)
{
    const QFileInfo targetInfo(targetPath);
    QDir targetDir = targetInfo.dir();
    if (!targetDir.exists() && !targetDir.mkpath(QStringLiteral("."))) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to create EPUB target directory: %1").arg(targetDir.absolutePath());
        }
        return {};
    }

    QTemporaryFile tempFile(targetDir.filePath(targetInfo.fileName() + QStringLiteral(".XXXXXX")));
    tempFile.setAutoRemove(false);
    if (!tempFile.open()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to create temporary EPUB: %1").arg(tempFile.errorString());
        }
        return {};
    }

    const QString tempPath = tempFile.fileName();
    if (tempFile.write(data) != data.size()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to write temporary EPUB: %1").arg(tempFile.errorString());
        }
        tempFile.close();
        QFile::remove(tempPath);
        return {};
    }

    tempFile.close();
    if (tempFile.error() != QFileDevice::NoError) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to finalize temporary EPUB: %1").arg(tempFile.errorString());
        }
        QFile::remove(tempPath);
        return {};
    }

    return tempPath;
}

static bool replaceFileWithBackup(const QString &replacementPath, const QString &targetPath, FileReplacementBackup &backup, QString *errorMessage)
{
    backup.targetPath = targetPath;
    backup.backupPath.clear();
    backup.hadOriginal = false;

    if (QFileInfo::exists(targetPath)) {
        backup.backupPath = targetPath + QStringLiteral(".bak-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QFile::rename(targetPath, backup.backupPath)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Unable to move existing EPUB aside: %1").arg(targetPath);
            }
            return false;
        }
        backup.hadOriginal = true;
    }

    if (!QFile::rename(replacementPath, targetPath)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unable to move anchored EPUB into place: %1").arg(targetPath);
        }
        if (backup.hadOriginal) {
            QFile::rename(backup.backupPath, targetPath);
        }
        QFile::remove(replacementPath);
        return false;
    }

    return true;
}

static bool rollbackFileReplacement(const FileReplacementBackup &backup, QString *errorMessage)
{
    if (backup.targetPath.isEmpty()) {
        return true;
    }

    bool rolledBack = true;
    QStringList errors;

    if (QFileInfo::exists(backup.targetPath) && !QFile::remove(backup.targetPath)) {
        rolledBack = false;
        errors.append(QStringLiteral("unable to remove new EPUB %1").arg(backup.targetPath));
    }

    if (backup.hadOriginal && !QFile::rename(backup.backupPath, backup.targetPath)) {
        rolledBack = false;
        errors.append(QStringLiteral("unable to restore previous EPUB %1").arg(backup.targetPath));
    }

    if (!rolledBack && errorMessage) {
        *errorMessage = errors.join(QStringLiteral("; "));
    }

    return rolledBack;
}

static void discardFileReplacementBackup(const FileReplacementBackup &backup)
{
    if (backup.hadOriginal && !backup.backupPath.isEmpty() && !QFile::remove(backup.backupPath)) {
        qWarning() << "Unable to remove EPUB replacement backup" << backup.backupPath;
    }
}

static bool isDocumentItem(const EpubItem &item)
{
    return item.mimetype == QByteArrayLiteral("application/xhtml+xml") || item.mimetype == QByteArrayLiteral("text/html")
        || item.mimetype == QByteArrayLiteral("application/x-dtbook+xml") || item.mimetype == QByteArrayLiteral("text/x-oeb1-document");
}

static QString elementLocalName(const QDomElement &element)
{
    const QString localName = element.localName().isEmpty() ? element.tagName() : element.localName();
    const qsizetype namespaceSeparator = localName.indexOf(QLatin1Char(':'));
    return (namespaceSeparator >= 0 ? localName.mid(namespaceSeparator + 1) : localName).toLower();
}

static bool isNonVisibleElement(const QDomElement &element)
{
    static const QSet<QString> names = {
        QStringLiteral("head"),
        QStringLiteral("meta"),
        QStringLiteral("link"),
        QStringLiteral("script"),
        QStringLiteral("style"),
        QStringLiteral("title"),
    };

    return names.contains(elementLocalName(element));
}

static void appendVisibleText(const QDomNode &node, QString &text)
{
    if (node.isText() || node.isCDATASection()) {
        text.append(node.nodeValue());
        text.append(QLatin1Char(' '));
        return;
    }

    const QDomElement element = node.toElement();
    if (!element.isNull() && isNonVisibleElement(element)) {
        return;
    }

    QDomNode child = node.firstChild();
    while (!child.isNull()) {
        appendVisibleText(child, text);
        child = child.nextSibling();
    }
}

static bool setDomContentWithNamespaces(QDomDocument &doc, const QByteArray &data)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    return static_cast<bool>(doc.setContent(data, QDomDocument::ParseOption::UseNamespaceProcessing));
#else
    return doc.setContent(data, true);
#endif
}

static QByteArray visibleTextDigestInput(const QByteArray &data)
{
    QDomDocument doc;
    if (!setDomContentWithNamespaces(doc, data)) {
        return data;
    }

    QString visibleText;
    appendVisibleText(doc.documentElement(), visibleText);
    visibleText.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    visibleText = visibleText.trimmed();
    return visibleText.toUtf8();
}

static void addHashField(QCryptographicHash &hash, const QByteArray &field)
{
    hash.addData(QByteArray::number(field.size()));
    hash.addData(QByteArrayLiteral("\n"));
    hash.addData(field);
    hash.addData(QByteArrayLiteral("\n"));
}

static QStringList documentItemIdsInHashOrder(const EPubContainer &container)
{
    QStringList itemIds = container.items();
    QStringList unorderedDocumentItemIds;
    const QHash<QString, EpubItem> manifestItems = container.manifestItems();
    for (auto it = manifestItems.cbegin(); it != manifestItems.cend(); ++it) {
        if (!itemIds.contains(it.key()) && isDocumentItem(it.value())) {
            unorderedDocumentItemIds.append(it.key());
        }
    }

    std::sort(unorderedDocumentItemIds.begin(), unorderedDocumentItemIds.end(), [&manifestItems](const QString &left, const QString &right) {
        return QString::localeAwareCompare(manifestItems.value(left).path, manifestItems.value(right).path) < 0;
    });
    itemIds.append(unorderedDocumentItemIds);

    return itemIds;
}

static BookHashes calculateHashes(const QString &filename)
{
    EPubContainer container(nullptr);
    if (!container.openFile(filename)) {
        BookHashes hashes;
        hashes.errorMessage = QStringLiteral("Unable to open EPUB for revision hashing: %1").arg(filename);
        return hashes;
    }

    QCryptographicHash textHash(QCryptographicHash::Sha256);
    textHash.addData(QByteArrayLiteral("arianna-epub-visible-text-sha256-v1\n"));

    const QHash<QString, EpubItem> manifestItems = container.manifestItems();
    for (const QString &itemId : documentItemIdsInHashOrder(container)) {
        const EpubItem item = manifestItems.value(itemId);
        if (!isDocumentItem(item)) {
            continue;
        }

        const QByteArray data = container.readData(item.path);
        if (data.isEmpty()) {
            BookHashes hashes;
            hashes.errorMessage = QStringLiteral("Unable to read EPUB document for revision hashing: %1").arg(item.path);
            return hashes;
        }

        addHashField(textHash, item.path.toUtf8());
        addHashField(textHash, visibleTextDigestInput(data));
    }

    const QString documentStateHash = container.getContentHash();
    if (documentStateHash.isEmpty()) {
        BookHashes hashes;
        hashes.errorMessage = QStringLiteral("Unable to calculate EPUB document state hash: %1").arg(filename);
        return hashes;
    }

    BookHashes hashes;
    hashes.success = true;
    hashes.textContentHash = textHash.result().toHex();
    hashes.documentStateHash = documentStateHash.toLatin1();
    return hashes;
}

static BookRevision makeInitialRevision(const QString &bookId, const BookHashes &hashes)
{
    BookRevision revision;
    revision.revisionId = QUuid::createUuidV7();
    revision.bookId = bookId;
    revision.textContentHash = hashes.textContentHash;
    revision.documentStateHash = hashes.documentStateHash;
    revision.changeType = QStringLiteral("import");
    return revision;
}
}

BookSnapshot BookTruthStore::openBook(const QString &bookId) const
{
    BookSnapshot snapshot;
    snapshot.bookId = bookId;

    const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(bookId);
    if (!entry) {
        snapshot.errorMessage = QStringLiteral("No book entry found for identifier: %1").arg(bookId);
        return snapshot;
    }

    snapshot.filename = authoritativeEpubPath(entry->filename, bookId);
    const QFileInfo fileInfo(snapshot.filename);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        snapshot.errorMessage = QStringLiteral("EPUB file does not exist: %1").arg(snapshot.filename);
        return snapshot;
    }

    const BookHashes hashes = calculateHashes(fileInfo.absoluteFilePath());
    if (!hashes.success) {
        snapshot.errorMessage = hashes.errorMessage;
        return snapshot;
    }

    snapshot.textContentHash = hashes.textContentHash;
    snapshot.documentStateHash = hashes.documentStateHash;

    std::optional<BookRevision> revision = currentRevision(bookId);
    if (!revision) {
        BookRevision initialRevision = makeInitialRevision(bookId, hashes);
        if (!BookDatabase::self().saveBookRevision(initialRevision)) {
            snapshot.errorMessage = QStringLiteral("Unable to persist initial book revision for: %1").arg(bookId);
            return snapshot;
        }

        revision = initialRevision;
    }

    snapshot.revision = revision;
    snapshot.success = true;
    return snapshot;
}

BookCommitResult BookTruthStore::createAnchor(const QString &bookId, const QString &cfiRange, const QUuid &expectedRevisionId)
{
    BookCommitResult result;

    if (bookId.isEmpty() || cfiRange.isEmpty()) {
        result.errorMessage = QStringLiteral("Unable to create annotation anchor without book id and CFI range");
        return result;
    }

    const auto entry = BookDatabase::self().loadEntryByUniqueIdentifier(bookId);
    if (!entry) {
        result.errorMessage = QStringLiteral("No book entry found for identifier: %1").arg(bookId);
        return result;
    }

    const BookSnapshot snapshot = openBook(bookId);
    if (!snapshot.success || !snapshot.revision) {
        result.errorMessage = snapshot.errorMessage.isEmpty() ? QStringLiteral("No current revision found for book: %1").arg(bookId) : snapshot.errorMessage;
        return result;
    }

    const BookRevision currentRevision = *snapshot.revision;
    result.oldRevisionId = currentRevision.revisionId;
    result.textContentHash = currentRevision.textContentHash;
    result.documentStateHash = currentRevision.documentStateHash;

    if (expectedRevisionId.isNull() || currentRevision.revisionId != expectedRevisionId) {
        result.conflict = true;
        result.errorMessage = QStringLiteral("Expected revision does not match current book revision");
        return result;
    }

    const QString outputPath = anchoredEpubPath(entry->filename, bookId);
    const QString inputPath = authoritativeEpubPath(entry->filename, bookId);
    const QUuid anchorUuid = QUuid::createUuidV7();
    const QString elementId = anchorElementId(anchorUuid);

    QByteArray anchoredEpub;
    {
        EPubContainer container(nullptr);
        if (!container.openFile(inputPath)) {
            result.errorMessage = QStringLiteral("Unable to open EPUB for annotation anchor creation: %1").arg(inputPath);
            return result;
        }

        anchoredEpub = container.createAnnotationAnchoredEpub(cfiRange, elementId);
    }

    if (anchoredEpub.isEmpty()) {
        result.errorMessage = QStringLiteral("Unable to create annotation anchor for selected CFI range");
        return result;
    }

    QString temporaryError;
    const QString temporaryPath = writeTemporaryEpub(outputPath, anchoredEpub, &temporaryError);
    if (temporaryPath.isEmpty()) {
        result.errorMessage = temporaryError;
        return result;
    }

    const BookHashes newHashes = calculateHashes(temporaryPath);
    if (!newHashes.success) {
        QFile::remove(temporaryPath);
        result.errorMessage = newHashes.errorMessage;
        return result;
    }

    BookRevision newRevision;
    newRevision.revisionId = QUuid::createUuidV7();
    newRevision.bookId = bookId;
    newRevision.parentRevisionId = currentRevision.revisionId;
    newRevision.textContentHash = newHashes.textContentHash;
    newRevision.documentStateHash = newHashes.documentStateHash;
    newRevision.changeType = QStringLiteral("annotation-anchor");
    newRevision.changedObjectId = anchorUuid;

    FileReplacementBackup backup;
    QString replacementError;
    if (!replaceFileWithBackup(temporaryPath, outputPath, backup, &replacementError)) {
        result.errorMessage = replacementError;
        return result;
    }

    if (!BookDatabase::self().saveBookRevision(newRevision)) {
        QString rollbackError;
        const bool rolledBack = rollbackFileReplacement(backup, &rollbackError);
        result.errorMessage = rolledBack ? QStringLiteral("Unable to persist annotation anchor revision")
                                         : QStringLiteral("Unable to persist annotation anchor revision; rollback failed: %1").arg(rollbackError);
        return result;
    }

    discardFileReplacementBackup(backup);

    result.success = true;
    result.newRevisionId = newRevision.revisionId;
    result.textContentHash = newRevision.textContentHash;
    result.documentStateHash = newRevision.documentStateHash;
    result.createdAnchorId = anchorUuid;
    return result;
}

std::optional<BookRevision> BookTruthStore::currentRevision(const QString &bookId) const
{
    return BookDatabase::self().currentBookRevision(bookId);
}
