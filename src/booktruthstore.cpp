// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "booktruthstore.h"

#include "bookdatabase.h"
#include "categoryentriesmodel.h"
#include "epubcontainer.h"

#include <QCryptographicHash>
#include <QDomDocument>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

namespace
{
struct BookHashes {
    bool success = false;
    QString errorMessage;
    QByteArray textContentHash;
    QByteArray documentStateHash;
};

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

    snapshot.filename = entry->filename;
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
    Q_UNUSED(cfiRange)

    BookCommitResult result;

    const std::optional<BookRevision> revision = currentRevision(bookId);
    if (!revision) {
        result.errorMessage = QStringLiteral("No current revision found for book: %1").arg(bookId);
        return result;
    }

    result.oldRevisionId = revision->revisionId;
    result.textContentHash = revision->textContentHash;
    result.documentStateHash = revision->documentStateHash;

    if (revision->revisionId != expectedRevisionId) {
        result.conflict = true;
        result.errorMessage = QStringLiteral("Expected revision does not match current book revision");
        return result;
    }

    result.errorMessage = QStringLiteral("Transactional anchor creation is implemented in the next refactoring step");
    return result;
}

std::optional<BookRevision> BookTruthStore::currentRevision(const QString &bookId) const
{
    return BookDatabase::self().currentBookRevision(bookId);
}
