// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "booklistmodel.h"

#include "bookdatabase.h"

#include <KFileMetaData/UserMetaData>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QMimeDatabase>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include "epubcontainer.h"

#include <QChar>
#include <QLoggingCategory>
#include <arianna_debug.h>

static QString normalizeBookServerIdentifier(QString identifier)
{
    identifier = identifier.trimmed();

    const QString uuidUrnPrefix = QStringLiteral("urn:uuid:");
    if (identifier.startsWith(uuidUrnPrefix, Qt::CaseInsensitive)) {
        identifier = identifier.mid(uuidUrnPrefix.size());
    }

    const QUuid uuid(identifier);
    if (!uuid.isNull()) {
        return uuid.toString(QUuid::WithoutBraces);
    }

    return identifier;
}

static void applyEpubIdentifiers(BookEntry &entry, EPubContainer &epub)
{
    const QStringList identifiers = epub.metadata(QStringLiteral("identifiers"));
    entry.identifier = identifiers.join(QStringLiteral(", "));
    entry.uniqueIdentifier = normalizeBookServerIdentifier(epub.metadata(QStringLiteral("unique-identifier")).value(0));
    if (entry.uniqueIdentifier.isEmpty()) {
        entry.uniqueIdentifier = normalizeBookServerIdentifier(identifiers.value(0));
    }
}

static bool refreshEpubIdentifiers(BookEntry &entry)
{
    QMimeDatabase db;
    if (db.mimeTypeForFile(entry.filename).name() != QStringLiteral("application/epub+zip")) {
        return false;
    }

    EPubContainer epub(nullptr);
    if (!epub.openFile(entry.filename)) {
        return false;
    }

    applyEpubIdentifiers(entry, epub);
    return !entry.uniqueIdentifier.isEmpty();
}

static bool refreshEpubThumbnail(BookEntry &entry)
{
    QMimeDatabase db;
    if (db.mimeTypeForFile(entry.filename).name() != QStringLiteral("application/epub+zip")) {
        return false;
    }

    EPubContainer epub(nullptr);
    if (!epub.openFile(entry.filename)) {
        return false;
    }

    const QString thumbnail = entry.saveCover(epub.coverImage());
    if (thumbnail.isEmpty()) {
        return false;
    }

    entry.thumbnail = thumbnail;
    return true;
}

static void removeCachedCover(const QString &thumbnail, const QString &replacement = {})
{
    if (thumbnail.isEmpty()) {
        return;
    }

    const QFileInfo thumbnailInfo(thumbnail);
    if (!thumbnailInfo.exists()) {
        return;
    }

    if (!replacement.isEmpty() && thumbnailInfo.absoluteFilePath() == QFileInfo(replacement).absoluteFilePath()) {
        return;
    }

    const QString coversPath = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).absoluteFilePath(QStringLiteral("covers"));
    const QString normalizedCoversPath = QDir(coversPath).absolutePath();
    if (!thumbnailInfo.absoluteFilePath().startsWith(normalizedCoversPath + QLatin1Char('/'))) {
        return;
    }

    QFile::remove(thumbnailInfo.absoluteFilePath());
}

static void applyEpubMetadata(BookEntry &entry, EPubContainer &epub, bool refreshCover)
{
    const auto titles = epub.metadata(QStringLiteral("title"));
    entry.title = titles.isEmpty() ? QFileInfo(entry.filename).completeBaseName() : titles[0];
    entry.author = epub.metadata(QStringLiteral("creator"));
    entry.rights = epub.metadata(QStringLiteral("rights")).join(QStringLiteral(", "));
    entry.source = epub.metadata(QStringLiteral("source")).join(QStringLiteral(", "));
    applyEpubIdentifiers(entry, epub);
    entry.language = epub.metadata(QStringLiteral("language")).join(QStringLiteral(", "));
    entry.genres = epub.metadata(QStringLiteral("subject"));
    entry.publisher = epub.metadata(QStringLiteral("publisher")).join(QStringLiteral(", "));

    entry.series.clear();
    entry.seriesNumbers.clear();
    entry.seriesVolumes.clear();
    const auto collections = epub.collections();
    for (const auto &collection : collections) {
        entry.series.append(collection.name);
        entry.seriesNumbers.append(QStringLiteral("0"));
        entry.seriesVolumes.append(QString::number(collection.position));
    }

    if (!refreshCover) {
        return;
    }

    const QString previousThumbnail = entry.thumbnail;
    const auto image = epub.coverImage();
    entry.thumbnail = entry.saveCover(image);
    removeCachedCover(previousThumbnail, entry.thumbnail);
}

static bool bookEntryDataMatches(const BookEntry &first, const BookEntry &second)
{
    return first.filename == second.filename && first.filetitle == second.filetitle && first.title == second.title && first.genres == second.genres
        && first.keywords == second.keywords && first.characters == second.characters && first.series == second.series
        && first.seriesNumbers == second.seriesNumbers && first.seriesVolumes == second.seriesVolumes && first.author == second.author
        && first.rights == second.rights && first.publisher == second.publisher && first.created == second.created
        && first.lastOpenedTime == second.lastOpenedTime && first.currentLocation == second.currentLocation && first.currentProgress == second.currentProgress
        && first.zoomLevel == second.zoomLevel && first.thumbnail == second.thumbnail && first.description == second.description
        && first.comment == second.comment && first.tags == second.tags && first.locations == second.locations && first.identifier == second.identifier
        && first.uniqueIdentifier == second.uniqueIdentifier && first.source == second.source && first.language == second.language
        && first.rating == second.rating;
}

class BookListModel::Private
{
public:
    Private()
        : contentModel(nullptr)
        , newlyAddedCategoryModel(nullptr)
        , authorCategoryModel(nullptr)
        , seriesCategoryModel(nullptr)
        , publisherCategoryModel(nullptr)
        , keywordCategoryModel(nullptr)
        , folderCategoryModel(nullptr)
        , cacheLoaded(false)
        , databaseSyncScheduled(false)
        , skipNextDatabaseSync(false) { };

    QList<BookEntry> entries;

    ContentList *contentModel;
    CategoryEntriesModel *newlyAddedCategoryModel;
    CategoryEntriesModel *authorCategoryModel;
    CategoryEntriesModel *seriesCategoryModel;
    CategoryEntriesModel *publisherCategoryModel;
    CategoryEntriesModel *keywordCategoryModel;
    CategoryEntriesModel *folderCategoryModel;

    bool cacheLoaded;
    bool databaseSyncScheduled;
    bool skipNextDatabaseSync;

    void initializeSubModels(BookListModel *q)
    {
        if (!newlyAddedCategoryModel) {
            newlyAddedCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, newlyAddedCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, newlyAddedCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->newlyAddedCategoryModelChanged();
        }
        if (!authorCategoryModel) {
            authorCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, authorCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, authorCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->authorCategoryModelChanged();
        }
        if (!seriesCategoryModel) {
            seriesCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, seriesCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, seriesCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->seriesCategoryModelChanged();
        }
        if (!publisherCategoryModel) {
            publisherCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, publisherCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, publisherCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->publisherCategoryModelChanged();
        }
        if (!keywordCategoryModel) {
            keywordCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, keywordCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, keywordCategoryModel, &CategoryEntriesModel::entryRemoved);
            Q_EMIT q->keywordCategoryModelChanged();
        }
        if (!folderCategoryModel) {
            folderCategoryModel = new CategoryEntriesModel(q);
            connect(q, &CategoryEntriesModel::entryDataUpdated, folderCategoryModel, &CategoryEntriesModel::entryDataUpdated);
            connect(q, &CategoryEntriesModel::entryRemoved, folderCategoryModel, &CategoryEntriesModel::entryRemoved);
            q->folderCategoryModel();
        }
    }

    bool addEntry(BookListModel *q, const BookEntry &entry)
    {
        if (q->indexOfFile(entry.filename) != -1) {
            return false;
        }

        entries.append(entry);
        q->append(entry);
        for (int i = 0; i < entry.author.size(); i++) {
            authorCategoryModel->addCategoryEntry(entry.author.at(i), entry);
        }
        for (int i = 0; i < entry.series.size(); i++) {
            seriesCategoryModel->addCategoryEntry(entry.series.at(i), entry, SeriesRole);
        }
        if (newlyAddedCategoryModel->indexOfFile(entry.filename) == -1) {
            newlyAddedCategoryModel->append(entry, CreatedRole);
        }
        publisherCategoryModel->addCategoryEntry(entry.publisher, entry);
        QUrl url(entry.filename.left(entry.filename.lastIndexOf(QLatin1Char('/'))));
        folderCategoryModel->addCategoryEntry(url.path().mid(1), entry);
        if (folderCategoryModel->indexOfFile(entry.filename) == -1) {
            folderCategoryModel->append(entry);
        }
        for (int i = 0; i < entry.genres.size(); i++) {
            keywordCategoryModel->addCategoryEntry(entry.genres.at(i), entry, GenreRole);
        }

        return true;
    }

    qsizetype entryIndexForFile(const QString &fileName) const
    {
        for (qsizetype i = 0; i < entries.size(); ++i) {
            if (entries.at(i).filename == fileName) {
                return i;
            }
        }

        return -1;
    }

    QList<BookEntry> loadValidEntries(bool refreshMissingMetadata)
    {
        QList<BookEntry> validEntries;
        const QList<BookEntry> cachedEntries = BookDatabase::self().loadEntries();
        QSet<QString> usedThumbnails;
        for (const BookEntry &entry : std::as_const(cachedEntries)) {
            /*
             * This might turn out a little slow, but we should avoid having entries
             * that do not exist. If we end up with slowdown issues when loading the
             * cache this would be a good place to start investigating.
             */
            if (QFileInfo::exists(entry.filename)) {
                BookEntry cachedEntry = entry;
                if (refreshMissingMetadata && cachedEntry.uniqueIdentifier.isEmpty() && refreshEpubIdentifiers(cachedEntry)) {
                    BookDatabase::self().updateEntry(cachedEntry.filename, QStringLiteral("identifier"), cachedEntry.identifier);
                    BookDatabase::self().updateEntry(cachedEntry.filename, QStringLiteral("uniqueIdentifier"), cachedEntry.uniqueIdentifier);
                }

                const QString thumbnailPath = cachedEntry.thumbnail.isEmpty() ? QString() : QFileInfo(cachedEntry.thumbnail).absoluteFilePath();
                const bool thumbnailNeedsRefresh =
                    cachedEntry.thumbnail.isEmpty() || !QFileInfo::exists(cachedEntry.thumbnail) || usedThumbnails.contains(thumbnailPath);
                if (refreshMissingMetadata && thumbnailNeedsRefresh && refreshEpubThumbnail(cachedEntry)) {
                    BookDatabase::self().updateEntry(cachedEntry.filename, QStringLiteral("thumbnail"), cachedEntry.thumbnail);
                }
                if (!cachedEntry.thumbnail.isEmpty()) {
                    usedThumbnails.insert(QFileInfo(cachedEntry.thumbnail).absoluteFilePath());
                }
                validEntries.append(cachedEntry);
            } else {
                BookDatabase::self().removeEntry(entry);
            }
        }

        return validEntries;
    }

    void loadCache(BookListModel *q)
    {
        const QList<BookEntry> validEntries = loadValidEntries(false);
        if (!validEntries.isEmpty()) {
            initializeSubModels(q);
        }

        int i = 0;
        for (const BookEntry &entry : validEntries) {
            addEntry(q, entry);
            if (++i % 100 == 0) {
                Q_EMIT q->countChanged();
                qApp->processEvents();
            }
        }

        cacheLoaded = true;
        Q_EMIT q->cacheLoadedChanged();
    }

    void syncWithDatabase(BookListModel *q)
    {
        const QList<BookEntry> databaseEntries = loadValidEntries(false);

        QHash<QString, BookEntry> databaseEntriesByFile;
        databaseEntriesByFile.reserve(databaseEntries.size());
        for (const BookEntry &entry : databaseEntries) {
            databaseEntriesByFile.insert(entry.filename, entry);
        }

        bool bookCountChanged = false;
        for (qsizetype i = entries.size() - 1; i >= 0; --i) {
            const BookEntry entry = entries.at(i);
            if (databaseEntriesByFile.contains(entry.filename)) {
                continue;
            }

            entries.removeAt(i);
            Q_EMIT q->entryRemoved(entry);
            bookCountChanged = true;
        }

        if (!databaseEntries.isEmpty()) {
            initializeSubModels(q);
        }

        for (const BookEntry &databaseEntry : databaseEntries) {
            const qsizetype existingIndex = entryIndexForFile(databaseEntry.filename);
            if (existingIndex < 0) {
                if (addEntry(q, databaseEntry)) {
                    bookCountChanged = true;
                }
                continue;
            }

            const BookEntry existingEntry = entries.at(existingIndex);
            if (bookEntryDataMatches(existingEntry, databaseEntry)) {
                continue;
            }

            entries.removeAt(existingIndex);
            Q_EMIT q->entryRemoved(existingEntry);
            addEntry(q, databaseEntry);
        }

        if (contentModel) {
            contentModel->setKnownFiles(q->knownBookFiles());
        }

        if (bookCountChanged) {
            Q_EMIT q->countChanged();
        }
    }
};

BookListModel::BookListModel(QObject *parent)
    : CategoryEntriesModel(parent)
    , d(std::make_unique<Private>())
{
    connect(&BookDatabase::self(), &BookDatabase::databaseChanged, this, &BookListModel::scheduleDatabaseSync);
}

BookListModel::~BookListModel() = default;

void BookListModel::componentComplete()
{
    QTimer::singleShot(0, this, [this]() {
        d->loadCache(this);
    });
}

void BookListModel::scheduleDatabaseSync()
{
    if (!d->cacheLoaded || d->databaseSyncScheduled) {
        return;
    }

    if (d->skipNextDatabaseSync) {
        d->skipNextDatabaseSync = false;
        return;
    }

    d->databaseSyncScheduled = true;
    QTimer::singleShot(250, this, [this]() {
        d->databaseSyncScheduled = false;
        if (!d->cacheLoaded) {
            return;
        }

        d->syncWithDatabase(this);
    });
}

bool BookListModel::cacheLoaded() const
{
    return d->cacheLoaded;
}

void BookListModel::setContentModel(ContentList *newModel)
{
    if (d->contentModel) {
        d->contentModel->disconnect(this);
    }
    d->contentModel = newModel;
    if (d->contentModel) {
        connect(d->contentModel, &QAbstractItemModel::rowsInserted, this, &BookListModel::contentModelItemsInserted);
    }
    Q_EMIT contentModelChanged();
}

ContentList *BookListModel::contentModel() const
{
    return d->contentModel;
}

void BookListModel::contentModelItemsInserted(QModelIndex index, int first, int last)
{
    d->initializeSubModels(this);

    // Process items in batches for better performance
    QList<BookEntry> newEntries;
    newEntries.reserve(last - first + 1);

    for (int i = first; i < last + 1; ++i) {
        QVariant filePath = d->contentModel->data(d->contentModel->index(i, 0, index), ContentList::FilePathRole);
        BookEntry entry;
        entry.filename = filePath.toUrl().toLocalFile();
        QStringList splitName = entry.filename.split(QLatin1Char('/'));
        if (!splitName.isEmpty())
            entry.filetitle = splitName.takeLast();
        if (!splitName.isEmpty()) {
            entry.series = QStringList{};
            entry.seriesNumbers = QStringList{QStringLiteral("0")};
            entry.seriesVolumes = QStringList{QStringLiteral("0")};
        }
        // just in case we end up without a title... using complete basename here,
        // as we would rather have "book one. part two" and the odd "book one - part two.tar"
        QFileInfo fileinfo(entry.filename);
        entry.title = fileinfo.completeBaseName();

        KFileMetaData::UserMetaData data(entry.filename);
        entry.rating = data.rating();
        entry.comment = data.userComment();
        entry.tags = data.tags();

        QVariantHash metadata = d->contentModel->data(d->contentModel->index(i, 0, index), Qt::UserRole + 2).toHash();
        QVariantHash::const_iterator it = metadata.constBegin();
        for (; it != metadata.constEnd(); it++) {
            if (it.key() == QLatin1String("author")) {
                entry.author = it.value().toStringList();
            } else if (it.key() == QLatin1String("title")) {
                entry.title = it.value().toString().trimmed();
            } else if (it.key() == QLatin1String("publisher")) {
                entry.publisher = it.value().toString().trimmed();
            } else if (it.key() == QLatin1String("created")) {
                entry.created = it.value().toDateTime();
            } else if (it.key() == QLatin1String("currentLocation")) {
                entry.currentLocation = it.value().toString();
            } else if (it.key() == QLatin1String("currentProgress")) {
                entry.currentProgress = it.value().toInt();
            } else if (it.key() == QLatin1String("comments")) {
                entry.comment = it.value().toString();
            } else if (it.key() == QLatin1String("tags")) {
                entry.tags = it.value().toStringList();
            } else if (it.key() == QLatin1String("rating")) {
                entry.rating = it.value().toInt();
            }
        }
        QMimeDatabase db;
        QString mimetype = db.mimeTypeForFile(entry.filename).name();
        if (mimetype == QStringLiteral("application/epub+zip")) {
            EPubContainer epub(nullptr);
            epub.openFile(entry.filename);
            const auto titles = epub.metadata(QStringLiteral("title"));
            if (!titles.isEmpty()) {
                entry.title = titles[0];
            }
            entry.author = epub.metadata(QStringLiteral("creator"));
            entry.rights = epub.metadata(QStringLiteral("rights")).join(QStringLiteral(", "));
            entry.source = epub.metadata(QStringLiteral("source")).join(QStringLiteral(", "));
            applyEpubIdentifiers(entry, epub);
            entry.language = epub.metadata(QStringLiteral("language")).join(QStringLiteral(", "));
            entry.genres = epub.metadata(QStringLiteral("subject"));
            entry.publisher = epub.metadata(QStringLiteral("publisher")).join(QStringLiteral(", "));

            auto image = epub.coverImage();
            entry.thumbnail = entry.saveCover(image);

            const auto collections = epub.collections();
            for (const auto &collection : collections) {
                entry.series.append(collection.name);
                entry.seriesVolumes.append(QString::number(collection.position));
            }
        }

        newEntries.append(entry);
    }

    // Batch process the entries
    for (const BookEntry &entry : std::as_const(newEntries)) {
        if (d->addEntry(this, entry)) {
            BookDatabase::self().addEntry(entry);
        }
    }

    Q_EMIT countChanged();
}

CategoryEntriesModel *BookListModel::newlyAddedCategoryModel() const
{
    return d->newlyAddedCategoryModel;
}

CategoryEntriesModel *BookListModel::authorCategoryModel() const
{
    return d->authorCategoryModel;
}

CategoryEntriesModel *BookListModel::seriesCategoryModel() const
{
    return d->seriesCategoryModel;
}

CategoryEntriesModel *BookListModel::seriesModelForEntry(const QString &fileName)
{
    for (const BookEntry &entry : std::as_const(d->entries)) {
        if (entry.filename == fileName) {
            return d->seriesCategoryModel->leafModelForEntry(entry);
        }
    }
    return nullptr;
}

CategoryEntriesModel *BookListModel::publisherCategoryModel() const
{
    return d->publisherCategoryModel;
}

CategoryEntriesModel *BookListModel::keywordCategoryModel() const
{
    return d->keywordCategoryModel;
}

CategoryEntriesModel *BookListModel::folderCategoryModel() const
{
    return d->folderCategoryModel;
}

int BookListModel::count() const
{
    return d->entries.count();
}

void BookListModel::setBookData(const QString &fileName, const QString &property, const QString &value)
{
    for (BookEntry &entry : d->entries) {
        if (entry.filename == fileName) {
            d->skipNextDatabaseSync = true;
            if (property == QStringLiteral("currentLocation")) {
                entry.currentLocation = value;
                BookDatabase::self().updateEntry(entry.filename, property, {value});
            } else if (property == QStringLiteral("currentProgress")) {
                entry.currentProgress = value.toInt();
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(value.toInt()));
            } else if (property == QStringLiteral("zoomLevel")) {
                entry.zoomLevel = value.toDouble();
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(entry.zoomLevel));
            } else if (property == QStringLiteral("locations")) {
                entry.locations = value;
                BookDatabase::self().updateEntry(entry.filename, property, {value});
            } else if (property == QStringLiteral("lastOpenedTime")) {
                entry.lastOpenedTime = QDateTime::fromString(value, Qt::ISODateWithMs);
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(entry.lastOpenedTime));
            } else if (property == QStringLiteral("rating")) {
                entry.rating = value.toInt();
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(value.toInt()));
            } else if (property == QStringLiteral("tags")) {
                entry.tags = value.split(QLatin1Char(','));
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(value.split(QLatin1Char(','))));
            } else if (property == QStringLiteral("comment")) {
                entry.comment = value;
                BookDatabase::self().updateEntry(entry.filename, property, QVariant(value));
            }
            Q_EMIT entryDataUpdated(entry);
            break;
        }
    }
}

BookEntry BookListModel::refreshBookFromFile(const QString &fileName, bool refreshCover)
{
    const QUrl fileUrl(fileName);
    const QString localFileName = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileName;
    const QFileInfo fileInfo(localFileName);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        return BookEntry();
    }

    qsizetype entryIndex = -1;
    const QString absoluteFileName = fileInfo.absoluteFilePath();
    for (qsizetype i = 0; i < d->entries.size(); ++i) {
        if (QFileInfo(d->entries.at(i).filename).absoluteFilePath() == absoluteFileName) {
            entryIndex = i;
            break;
        }
    }

    if (entryIndex < 0) {
        return BookEntry();
    }

    QMimeDatabase db;
    if (db.mimeTypeForFile(fileInfo).name() != QStringLiteral("application/epub+zip")) {
        return BookEntry();
    }

    EPubContainer epub(nullptr);
    if (!epub.openFile(absoluteFileName)) {
        return BookEntry();
    }

    const BookEntry oldEntry = d->entries.at(entryIndex);
    BookEntry refreshedEntry = oldEntry;
    refreshedEntry.filetitle = QFileInfo(refreshedEntry.filename).fileName();
    applyEpubMetadata(refreshedEntry, epub, refreshCover);

    d->entries.removeAt(entryIndex);
    Q_EMIT entryRemoved(oldEntry);

    d->initializeSubModels(this);
    d->addEntry(this, refreshedEntry);
    d->skipNextDatabaseSync = true;
    BookDatabase::self().updateEntry(refreshedEntry);

    qCDebug(ARIANNA_LOG) << "Refreshed book metadata from edited EPUB" << refreshedEntry.filename;

    return refreshedEntry;
}

void BookListModel::removeBook(const QString &fileName, bool deleteFile)
{
    for (qsizetype i = 0; i < d->entries.size(); ++i) {
        const BookEntry entry = d->entries.at(i);
        if (entry.filename != fileName) {
            continue;
        }

        if (deleteFile && !QFile::remove(entry.filename)) {
            qCWarning(ARIANNA_LOG) << "Could not delete book file" << entry.filename;
        }

        d->entries.removeAt(i);
        Q_EMIT entryRemoved(entry);
        Q_EMIT countChanged();
        BookDatabase::self().removeEntry(entry);
        break;
    }
}

QStringList BookListModel::knownBookFiles() const
{
    QStringList files;
    for (const auto &entry : std::as_const(d->entries)) {
        files.append(entry.filename);
    }
    return files;
}

#include "moc_booklistmodel.cpp"
