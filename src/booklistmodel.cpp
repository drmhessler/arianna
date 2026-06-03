// SPDX-FileCopyrightText: 2015 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "booklistmodel.h"

#include "bookdatabase.h"

#include <KFileMetaData/UserMetaData>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QMimeDatabase>
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
        , cacheLoaded(false) {};

    QList<BookEntry> entries;

    ContentList *contentModel;
    CategoryEntriesModel *newlyAddedCategoryModel;
    CategoryEntriesModel *authorCategoryModel;
    CategoryEntriesModel *seriesCategoryModel;
    CategoryEntriesModel *publisherCategoryModel;
    CategoryEntriesModel *keywordCategoryModel;
    CategoryEntriesModel *folderCategoryModel;

    bool cacheLoaded;

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

    void loadCache(BookListModel *q)
    {
        QList<BookEntry> entries = BookDatabase::self().loadEntries();
        if (!entries.isEmpty()) {
            initializeSubModels(q);
        }
        int i = 0;
        for (const BookEntry &entry : std::as_const(entries)) {
            /*
             * This might turn out a little slow, but we should avoid having entries
             * that do not exist. If we end up with slowdown issues when loading the
             * cache this would be a good place to start investigating.
             */
            if (QFileInfo::exists(entry.filename)) {
                BookEntry cachedEntry = entry;
                if (cachedEntry.uniqueIdentifier.isEmpty() && refreshEpubIdentifiers(cachedEntry)) {
                    BookDatabase::self().updateEntry(cachedEntry.filename, QStringLiteral("identifier"), cachedEntry.identifier);
                    BookDatabase::self().updateEntry(cachedEntry.filename, QStringLiteral("uniqueIdentifier"), cachedEntry.uniqueIdentifier);
                }
                addEntry(q, cachedEntry);
                if (++i % 100 == 0) {
                    Q_EMIT q->countChanged();
                    qApp->processEvents();
                }
            } else {
                BookDatabase::self().removeEntry(entry);
            }
        }
        cacheLoaded = true;
        Q_EMIT q->cacheLoadedChanged();
    }
};

BookListModel::BookListModel(QObject *parent)
    : CategoryEntriesModel(parent)
    , d(std::make_unique<Private>())
{
}

BookListModel::~BookListModel() = default;

void BookListModel::componentComplete()
{
    QTimer::singleShot(0, this, [this]() {
        d->loadCache(this);
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

            auto image = epub.image(epub.metadata(QStringLiteral("cover")).join(QChar()));
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
