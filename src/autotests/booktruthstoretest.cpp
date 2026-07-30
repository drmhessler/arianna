// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "booktruthstore.h"
#include "bookdatabase.h"
#include "categoryentriesmodel.h"

#include <KZip>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

class BookTruthStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void testOpenBookCreatesInitialRevision();
    void testTextContentHashIgnoresTechnicalAnchors();
    void testVisibleTextChangeChangesBothHashes();
    void testCreateAnchorCreatesChildRevision();
    void testStaleExpectedRevisionConflicts();
    void testFailedAnchorDoesNotCreateRevisionOrEpub();
    void testRevisionRecordsAreImmutable();
};

static QString uniqueBookId(const QString &prefix)
{
    return prefix + QStringLiteral("-") + QUuid::createUuidV7().toString(QUuid::WithoutBraces);
}

static bool isUuidV7(const QUuid &uuid)
{
    const QString text = uuid.toString(QUuid::WithoutBraces);
    return text.size() > 14 && text.at(14) == QLatin1Char('7');
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

static void writeContainer(KZip &zip)
{
    zip.writeFile(QStringLiteral("META-INF/container.xml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/package.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)"));
}

static QByteArray packageDocument()
{
    return QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">truth-book</dc:identifier>
    <dc:title>Truth Store Test</dc:title>
  </metadata>
  <manifest>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine><itemref idref="chapter"/></spine>
</package>)");
}

static bool writeTestEpub(const QString &epubPath, const QByteArray &chapterDocument)
{
    KZip zip(epubPath);
    if (!zip.open(QIODevice::WriteOnly)) {
        return false;
    }

    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), packageDocument());
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), chapterDocument);

    return zip.close();
}

static QByteArray chapterWithBody(const QByteArray &body)
{
    return QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Ignored title</title></head><body>)")
        + body + QByteArray(R"(</body></html>)");
}

static void addBookEntry(const QString &bookId, const QString &epubPath)
{
    BookEntry entry;
    entry.filename = epubPath;
    entry.filetitle = QFileInfo(epubPath).fileName();
    entry.title = bookId;
    entry.identifier = bookId;
    entry.uniqueIdentifier = bookId;
    entry.created = QDateTime::currentDateTimeUtc();
    BookDatabase::self().addEntry(entry);
}

void BookTruthStoreTest::initTestCase()
{
    const QString dataHome = QDir::temp().filePath(QStringLiteral("arianna-booktruthstoretest-%1").arg(QCoreApplication::applicationPid()));
    QVERIFY(QDir().mkpath(dataHome));
    qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome));

    QCoreApplication::setOrganizationName(QStringLiteral("KDE"));
    QCoreApplication::setApplicationName(QStringLiteral("arianna-booktruthstoretest"));

    QDir location(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    QVERIFY(location.mkpath(QStringLiteral(".")));
    QFile::remove(location.filePath(QStringLiteral("library.sqlite")));
}

void BookTruthStoreTest::testOpenBookCreatesInitialRevision()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("initial"));
    const QString epubPath = dir.filePath(QStringLiteral("initial.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Initial visible text.</p>"))));
    addBookEntry(bookId, epubPath);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));
    QVERIFY(snapshot.revision.has_value());
    QVERIFY(isUuidV7(snapshot.revision->revisionId));
    QCOMPARE(snapshot.revision->bookId, bookId);
    QVERIFY(!snapshot.revision->parentRevisionId.has_value());
    QCOMPARE(snapshot.revision->changeType, QStringLiteral("import"));
    QCOMPARE(snapshot.revision->textContentHash, snapshot.textContentHash);
    QCOMPARE(snapshot.revision->documentStateHash, snapshot.documentStateHash);

    const QList<BookRevision> revisions = BookDatabase::self().bookRevisions(bookId);
    QCOMPARE(revisions.size(), 1);
    QCOMPARE(revisions.constFirst().revisionId, snapshot.revision->revisionId);

    const BookSnapshot secondSnapshot = store.openBook(bookId);
    QVERIFY2(secondSnapshot.success, qPrintable(secondSnapshot.errorMessage));
    QCOMPARE(BookDatabase::self().bookRevisions(bookId).size(), 1);
    QCOMPARE(secondSnapshot.revision->revisionId, snapshot.revision->revisionId);
}

void BookTruthStoreTest::testTextContentHashIgnoresTechnicalAnchors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString plainBookId = uniqueBookId(QStringLiteral("plain"));
    const QString anchoredBookId = uniqueBookId(QStringLiteral("anchored"));
    const QString plainPath = dir.filePath(QStringLiteral("plain.epub"));
    const QString anchoredPath = dir.filePath(QStringLiteral("anchored.epub"));

    QVERIFY(writeTestEpub(plainPath, chapterWithBody(QByteArrayLiteral("<p>Visible truth text.</p>"))));
    QVERIFY(writeTestEpub(anchoredPath,
                          chapterWithBody(QByteArrayLiteral("<p><span id=\"anchor_begin\" data-role=\"anchor\" data-anchor-type=\"annotation-begin\"></span>"
                                                            "Visible truth text."
                                                            "<span id=\"anchor_end\" data-role=\"anchor\" data-anchor-type=\"annotation-end\"></span></p>"))));
    addBookEntry(plainBookId, plainPath);
    addBookEntry(anchoredBookId, anchoredPath);

    BookTruthStore store;
    const BookSnapshot plain = store.openBook(plainBookId);
    const BookSnapshot anchored = store.openBook(anchoredBookId);
    QVERIFY2(plain.success, qPrintable(plain.errorMessage));
    QVERIFY2(anchored.success, qPrintable(anchored.errorMessage));

    QCOMPARE(anchored.textContentHash, plain.textContentHash);
    QVERIFY(anchored.documentStateHash != plain.documentStateHash);
}

void BookTruthStoreTest::testVisibleTextChangeChangesBothHashes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString firstBookId = uniqueBookId(QStringLiteral("visible-a"));
    const QString secondBookId = uniqueBookId(QStringLiteral("visible-b"));
    const QString firstPath = dir.filePath(QStringLiteral("visible-a.epub"));
    const QString secondPath = dir.filePath(QStringLiteral("visible-b.epub"));

    QVERIFY(writeTestEpub(firstPath, chapterWithBody(QByteArrayLiteral("<p>Original visible text.</p>"))));
    QVERIFY(writeTestEpub(secondPath, chapterWithBody(QByteArrayLiteral("<p>Changed visible text.</p>"))));
    addBookEntry(firstBookId, firstPath);
    addBookEntry(secondBookId, secondPath);

    BookTruthStore store;
    const BookSnapshot first = store.openBook(firstBookId);
    const BookSnapshot second = store.openBook(secondBookId);
    QVERIFY2(first.success, qPrintable(first.errorMessage));
    QVERIFY2(second.success, qPrintable(second.errorMessage));

    QVERIFY(first.textContentHash != second.textContentHash);
    QVERIFY(first.documentStateHash != second.documentStateHash);
}

void BookTruthStoreTest::testCreateAnchorCreatesChildRevision()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("anchor"));
    const QString epubPath = dir.filePath(QStringLiteral("anchor.epub"));
    const QByteArray chapterDocument = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p>Hello anchored truth.</p></body></html>");
    QVERIFY(writeTestEpub(epubPath, chapterDocument));
    addBookEntry(bookId, epubPath);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));

    const BookCommitResult result = store.createAnchor(bookId, QStringLiteral("epubcfi(/6/2!/2/2,/1:6,/1:14)"), snapshot.revision->revisionId);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.createdAnchorId.has_value());
    QVERIFY(isUuidV7(*result.createdAnchorId));
    QVERIFY(isUuidV7(result.newRevisionId));
    QCOMPARE(result.oldRevisionId, snapshot.revision->revisionId);
    QVERIFY(result.newRevisionId != snapshot.revision->revisionId);
    QCOMPARE(result.textContentHash, snapshot.textContentHash);
    QVERIFY(result.documentStateHash != snapshot.documentStateHash);

    const QList<BookRevision> revisions = BookDatabase::self().bookRevisions(bookId);
    QCOMPARE(revisions.size(), 2);
    QVERIFY(revisions.constLast().parentRevisionId.has_value());
    QCOMPARE(*revisions.constLast().parentRevisionId, snapshot.revision->revisionId);
    QCOMPARE(revisions.constLast().revisionId, result.newRevisionId);
    QVERIFY(revisions.constLast().changedObjectId.has_value());
    QCOMPARE(*revisions.constLast().changedObjectId, *result.createdAnchorId);
    QCOMPARE(revisions.constLast().changeType, QStringLiteral("annotation-anchor"));

    const QString anchoredPath = anchoredEpubPath(epubPath, bookId);
    QVERIFY(QFileInfo::exists(anchoredPath));

    KZip anchoredZip(anchoredPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());
    const QString anchorElementId = QStringLiteral("uuid_") + result.createdAnchorId->toString(QUuid::WithoutBraces);
    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorElementId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation\"")));
    QVERIFY(document.contains(QStringLiteral(">anchored</span>")));
}

void BookTruthStoreTest::testStaleExpectedRevisionConflicts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("conflict"));
    const QString epubPath = dir.filePath(QStringLiteral("conflict.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Conflict visible text.</p>"))));
    addBookEntry(bookId, epubPath);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));

    QUuid staleRevisionId = QUuid::createUuidV7();
    QVERIFY(staleRevisionId != snapshot.revision->revisionId);

    const BookCommitResult result = store.createAnchor(bookId, QStringLiteral("epubcfi(/6/2!/2/2,/1:0,/1:8)"), staleRevisionId);
    QVERIFY(!result.success);
    QVERIFY(result.conflict);
    QCOMPARE(result.oldRevisionId, snapshot.revision->revisionId);
    QCOMPARE(BookDatabase::self().bookRevisions(bookId).size(), 1);
    QVERIFY(!QFileInfo::exists(anchoredEpubPath(epubPath, bookId)));
}

void BookTruthStoreTest::testFailedAnchorDoesNotCreateRevisionOrEpub()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("failed-anchor"));
    const QString epubPath = dir.filePath(QStringLiteral("failed-anchor.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Failed anchor visible text.</p>"))));
    addBookEntry(bookId, epubPath);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));

    const BookCommitResult result = store.createAnchor(bookId, QStringLiteral("epubcfi(/6/2!/2/2/1:6)"), snapshot.revision->revisionId);
    QVERIFY(!result.success);
    QVERIFY(!result.conflict);
    QCOMPARE(BookDatabase::self().bookRevisions(bookId).size(), 1);
    QVERIFY(!QFileInfo::exists(anchoredEpubPath(epubPath, bookId)));
}

void BookTruthStoreTest::testRevisionRecordsAreImmutable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString bookId = uniqueBookId(QStringLiteral("immutable"));
    const QString epubPath = dir.filePath(QStringLiteral("immutable.epub"));
    QVERIFY(writeTestEpub(epubPath, chapterWithBody(QByteArrayLiteral("<p>Immutable visible text.</p>"))));
    addBookEntry(bookId, epubPath);

    BookTruthStore store;
    const BookSnapshot snapshot = store.openBook(bookId);
    QVERIFY2(snapshot.success, qPrintable(snapshot.errorMessage));

    BookRevision mutated = *snapshot.revision;
    mutated.textContentHash = QByteArrayLiteral("changed");
    mutated.documentStateHash = QByteArrayLiteral("changed");
    mutated.changeType = QStringLiteral("mutated");
    QVERIFY(!BookDatabase::self().saveBookRevision(mutated));

    const QList<BookRevision> revisions = BookDatabase::self().bookRevisions(bookId);
    QCOMPARE(revisions.size(), 1);
    QCOMPARE(revisions.constFirst().revisionId, snapshot.revision->revisionId);
    QCOMPARE(revisions.constFirst().textContentHash, snapshot.revision->textContentHash);
    QCOMPARE(revisions.constFirst().changeType, QStringLiteral("import"));
}

QTEST_MAIN(BookTruthStoreTest)
#include "booktruthstoretest.moc"
