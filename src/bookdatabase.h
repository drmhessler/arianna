// SPDX-FileCopyrightText: 2017 Dan Leinir Turthra Jensen <admin@leinir.dk>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "bookrevision.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <optional>

struct BookEntry;
/**
 * \brief A Class to hold a cache of known books to reduce the amount of time spent indexing.
 *
 * BookDatabase handles holding the conversion between SQL entry and
 * BookEntry structs.
 *
 * The BookEntry struct is defined in CategoryEntriesModel.
 */
class BookDatabase : public QObject
{
    Q_OBJECT
public:
    static BookDatabase &self()
    {
        static BookDatabase instance;
        return instance;
    }
    // ********************************************
    // *             Book Management              *
    // ********************************************

    /// @return a list of all known books in the database.
    QList<BookEntry> loadEntries();

    /// @return an entry matching the file name if it exists.
    std::optional<BookEntry> loadEntry(const QString &fileName);
    std::optional<BookEntry> loadEntryByIdentifier(const QString &identifier);
    std::optional<BookEntry> loadEntryByUniqueIdentifier(const QString &uniqueIdentifier);

    /// \brief Add a new book to the cache.
    /// \param entry The entry to add.
    void addEntry(const BookEntry &entry);

    /// \brief Update all stored fields for an existing book.
    /// \param entry The entry to update.
    void updateEntry(const BookEntry &entry);

    /// \brief remove an entry by filename from the cache.
    /// \param entry the entry to remove.
    void removeEntry(const BookEntry &entry);

    /// \brief updateEntry update an entry by filename.
    /// \param fileName the filename of the entry to update.
    /// \param property the property/fieldname you wish to update.
    /// \param value a QVariant with the value.
    void updateEntry(const QString &fileName, const QString &property, const QVariant &value);

    // ********************************************
    // *   Annotation and Reference Management    *
    // ********************************************

    /// @return saved annotations for a book file.
    QVariantList loadAnnotations(const QString &bookId);

    /// Add or update an annotation for a book file.
    void saveAnnotation(const QString &bookId, const QVariantMap &annotation);

    /// Remove an annotation for a book file.
    void removeAnnotation(const QString &bookId, const QString &value);

    /// @return saved references for a book file.
    QVariantList loadReferences(const QString &bookId);

    /// @return reference for given by source book and anchor.
    QVariantMap loadReferenceBySource(const QString &bookId, const QString &sourceAnchorId);

    /// Add or update an reference between two books
    void saveReference(const QVariantMap &reference);

    void removeReference(const QString &referenceId, const QString &sourceBookId, const QString &targetBookId, const QString &targetAnchorId);

    /// @return the current leaf revision for a book if revision tracking has been initialized.
    std::optional<BookRevision> currentBookRevision(const QString &bookId);

    /// Insert an immutable book revision record. Existing records are never updated.
    bool saveBookRevision(const BookRevision &revision);

    /// @return all persisted revisions for a book in insertion order.
    QList<BookRevision> bookRevisions(const QString &bookId);

Q_SIGNALS:
    /// \brief Fires when the library database file was changed by this or another process.
    void databaseChanged();

private:
    explicit BookDatabase(QObject *parent = nullptr);
    ~BookDatabase() override;

    class Private;
    std::unique_ptr<Private> d;
};
