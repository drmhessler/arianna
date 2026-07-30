// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "epubcontainer.h"

#include <KArchiveDirectory>
#include <KArchiveFile>
#include <KZip>

#include <QBuffer>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QImage>
#include <QObject>
#include <QScopedPointer>
#include <QTemporaryDir>
#include <QTest>

class EPubContainerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testUniqueIdentifierMetadata();
    void testCoverImageFromMetadataCover();
    void testCoverImageFromManifestProperty();
    void testCoverImageFromPercentEncodedManifestHref();
    void testCoverImageFromGuideCoverPage();
    void testCreateAnchorWritesAnchoredCopy();
    void testCreateAnchorWrapsInlineSiblingRange();
    void testCreateAnchorRejectsBlockRange();
    void testCreateAnnotationAnchorAnnotatesWholeInlineElement();
    void testCreateAnnotationAnchorWrapsTextRange();
    void testCreateAnnotationAnchorCreatesBoundaryPair();
    void testCreateBookrefAnchorCreatesIntraHref();
    void testFileHashMatchesEpubBytes();
    void testContentHashIsStableAcrossZipEntryOrder();
    void testCreateTargetAnchorIsReferenceable();
    void testServerReadyEpubCanIncludeReferencesDocument();
    void testReferenceableTargetsIncludeDocumentsAndElementIds();
};

static QByteArray testPngData()
{
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(Qt::red);

    QByteArray data;
    QBuffer buffer(&data);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return data;
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

static QByteArray hashTestPackage()
{
    return QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="bookid">bookid</dc:identifier>
    <dc:title>Hash Test</dc:title>
  </metadata>
  <manifest>
    <item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/>
    <item id="style" href="style.css" media-type="text/css"/>
  </manifest>
  <spine><itemref idref="chapter"/></spine>
</package>)");
}

static QByteArray hashTestChapter(const QByteArray &bodyText)
{
    return QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>)")
        + bodyText + QByteArray(R"(</p></body></html>)");
}

static bool writeHashTestEpub(const QString &epubPath, bool reverseOrder, const QByteArray &bodyText)
{
    KZip zip(epubPath);
    if (!zip.open(QIODevice::WriteOnly)) {
        return false;
    }

    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));

    if (reverseOrder) {
        zip.writeFile(QStringLiteral("OEBPS/style.css"), QByteArrayLiteral("p { font-style: italic; }\n"));
        zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), hashTestChapter(bodyText));
        zip.writeFile(QStringLiteral("OEBPS/package.opf"), hashTestPackage());
        writeContainer(zip);
    } else {
        writeContainer(zip);
        zip.writeFile(QStringLiteral("OEBPS/package.opf"), hashTestPackage());
        zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), hashTestChapter(bodyText));
        zip.writeFile(QStringLiteral("OEBPS/style.css"), QByteArrayLiteral("p { font-style: italic; }\n"));
    }

    return zip.close();
}

void EPubContainerTest::testUniqueIdentifierMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("book.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="unique-identifier" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="unique-identifier">040d189b-0546-410e-a09d-c0c98be894aa</dc:identifier>
    <dc:identifier>urn:isbn:9783161484100</dc:identifier>
    <dc:title>Identifier Test</dc:title>
  </metadata>
</package>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    QStringList expectedIdentifiers;
    expectedIdentifiers << QStringLiteral("040d189b-0546-410e-a09d-c0c98be894aa") << QStringLiteral("urn:isbn:9783161484100");

    QCOMPARE(epub.metadata(QStringLiteral("identifiers")), expectedIdentifiers);
    QCOMPARE(epub.metadata(QStringLiteral("identifier")), expectedIdentifiers);
    QCOMPARE(epub.metadata(QStringLiteral("unique-identifier")).value(0), QStringLiteral("040d189b-0546-410e-a09d-c0c98be894aa"));
}

void EPubContainerTest::testCoverImageFromMetadataCover()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("metadata-cover.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Metadata Cover Test</dc:title>
    <meta name="cover" content="cover-image"/>
  </metadata>
  <manifest>
    <item id="cover-image" href="images/cover.png" media-type="image/png"/>
  </manifest>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/images/cover.png"), testPngData());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(!epub.coverImage().isNull());
}

void EPubContainerTest::testCoverImageFromManifestProperty()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("manifest-cover.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Manifest Cover Test</dc:title>
  </metadata>
  <manifest>
    <item id="cover" href="images/cover.png" media-type="image/png" properties="cover-image"/>
  </manifest>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/images/cover.png"), testPngData());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(!epub.coverImage().isNull());
}

void EPubContainerTest::testCoverImageFromPercentEncodedManifestHref()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("encoded-manifest-cover.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Encoded Manifest Cover Test</dc:title>
  </metadata>
  <manifest>
    <item id="cover" href="Images/Blake%2C%20William%20-%20Complete%20Poems%20%28Penguin%2C%202004%29.png" media-type="image/png" properties="cover-image"/>
  </manifest>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/Images/Blake, William - Complete Poems (Penguin, 2004).png"), testPngData());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QCOMPARE(epub.epubItem(QStringLiteral("cover")).path, QStringLiteral("OEBPS/Images/Blake, William - Complete Poems (Penguin, 2004).png"));
    QVERIFY(!epub.coverImage().isNull());
}

void EPubContainerTest::testCoverImageFromGuideCoverPage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("guide-cover.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Guide Cover Test</dc:title>
  </metadata>
  <manifest>
    <item id="cover-page" href="cover.xhtml" media-type="application/xhtml+xml"/>
    <item id="cover-image" href="images/cover.png" media-type="image/png"/>
  </manifest>
  <guide>
    <reference type="cover" href="cover.xhtml"/>
  </guide>
</package>)"));
    zip.writeFile(QStringLiteral("OEBPS/cover.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <body><img src="images/cover.png"/></body>
</html>)"));
    zip.writeFile(QStringLiteral("OEBPS/images/cover.png"), testPngData());
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));
    QVERIFY(!epub.coverImage().isNull());
}

void EPubContainerTest::testCreateAnchorWritesAnchoredCopy()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Anchor Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hello anchor world</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/1:6,/1:12)"));
    QVERIFY(!anchorId.isEmpty());
    QVERIFY(anchorId.startsWith(QStringLiteral("uuid_")));

    const QString anchoredPath = dir.filePath(QStringLiteral("bookid.anchored.epub"));
    QVERIFY(QFileInfo::exists(anchoredPath));

    KZip anchoredZip(anchoredPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
    QVERIFY(document.contains(QStringLiteral(">anchor</a>")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testCreateAnchorWrapsInlineSiblingRange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("inline-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Inline Anchor Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body epub:type="bodymatter"><p><span class="calibre17">Srimad-Bhagavatam,</span> Third Canto, 29th Chapter, 10th verse</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/2/1:0,/3:12)"));
    QVERIFY(!anchorId.isEmpty());

    const QString anchoredPath = dir.filePath(QStringLiteral("bookid.anchored.epub"));
    QVERIFY(QFileInfo::exists(anchoredPath));

    KZip anchoredZip(anchoredPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
    QVERIFY(document.contains(QStringLiteral("<a")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
    QVERIFY(document.contains(QStringLiteral("xmlns:epub=\"http://www.idpf.org/2007/ops\"")));
    QVERIFY(document.contains(QStringLiteral("epub:type=\"bodymatter\"")));

    const qsizetype anchorIndex = document.indexOf(QStringLiteral("id=\"") + anchorId + QStringLiteral("\""));
    const qsizetype spanTextIndex = document.indexOf(QStringLiteral("Srimad-Bhagavatam,"));
    const qsizetype followingTextIndex = document.indexOf(QStringLiteral("Third Canto"));
    const qsizetype anchorEndIndex = document.indexOf(QStringLiteral("</a>"), followingTextIndex);
    QVERIFY(anchorIndex >= 0);
    QVERIFY(anchorIndex < spanTextIndex);
    QVERIFY(spanTextIndex < followingTextIndex);
    QVERIFY(followingTextIndex < anchorEndIndex);
    QVERIFY(document.indexOf(QStringLiteral(", 29th Chapter"), anchorEndIndex) > anchorEndIndex);
}

void EPubContainerTest::testCreateAnchorRejectsBlockRange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("block-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Block Anchor Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>First paragraph</p><p>Second paragraph</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnchor(QStringLiteral("epubcfi(/6/2!/2,/2/1:6,/4/1:6)"));
    QVERIFY(anchorId.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("bookid.anchored.epub"))));
}

void EPubContainerTest::testCreateAnnotationAnchorAnnotatesWholeInlineElement()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("annotation-inline-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Annotation Inline Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p><span class="term">Bhakti</span> devotion</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnnotationAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/2/1:0,/2/1:6)"));
    QVERIFY(!anchorId.isEmpty());

    KZip anchoredZip(dir.filePath(QStringLiteral("bookid.anchored.epub")));
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("class=\"term\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation\"")));
    QVERIFY(!document.contains(anchorId + QStringLiteral("_begin")));
    QVERIFY(!document.contains(anchorId + QStringLiteral("_end")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testCreateAnnotationAnchorWrapsTextRange()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("annotation-text-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Annotation Text Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hello annotation world</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnnotationAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/1:6,/1:16)"));
    QVERIFY(!anchorId.isEmpty());

    KZip anchoredZip(dir.filePath(QStringLiteral("bookid.anchored.epub")));
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation\"")));
    QVERIFY(document.contains(QStringLiteral(">annotation</span>")));
    QVERIFY(!document.contains(anchorId + QStringLiteral("_begin")));
    QVERIFY(!document.contains(anchorId + QStringLiteral("_end")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testCreateAnnotationAnchorCreatesBoundaryPair()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("annotation-pair-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Annotation Pair Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hello <em>annotation</em> world</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnnotationAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/1:3,/3:4)"));
    QVERIFY(!anchorId.isEmpty());

    KZip anchoredZip(dir.filePath(QStringLiteral("bookid.anchored.epub")));
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    const QString beginId = anchorId + QStringLiteral("_begin");
    const QString endId = anchorId + QStringLiteral("_end");
    QVERIFY(document.contains(QStringLiteral("id=\"") + beginId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("id=\"") + endId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation-begin\"")));
    QVERIFY(document.contains(QStringLiteral("data-anchor-type=\"annotation-end\"")));
    QVERIFY(document.indexOf(beginId) < document.indexOf(QStringLiteral("<em>annotation</em>")));
    QVERIFY(document.indexOf(QStringLiteral("<em>annotation</em>")) < document.indexOf(endId));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testCreateBookrefAnchorCreatesIntraHref()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("intra-bookref-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Intra Bookref Test</dc:title></metadata><manifest><item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/><item id="chapter2" href="chapter2.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter1"/><itemref idref="chapter2"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter1.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>See target now</p></body></html>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter2.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p id="target">Target paragraph</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createBookrefAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/1:4,/1:10)"), QStringLiteral("OEBPS/chapter2.xhtml#target"), false);
    QVERIFY(!anchorId.isEmpty());

    KZip anchoredZip(dir.filePath(QStringLiteral("bookid.anchored.epub")));
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter1.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());

    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(document.contains(QStringLiteral("href=\"chapter2.xhtml#target\"")));
    QVERIFY(!document.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
    QVERIFY(document.contains(QStringLiteral(">target</a>")));
    QCOMPARE(document.count(QStringLiteral("http://www.w3.org/1999/xhtml")), 1);
}

void EPubContainerTest::testFileHashMatchesEpubBytes()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("hash-source.epub"));
    QVERIFY(writeHashTestEpub(epubPath, false, QByteArrayLiteral("Chain of truth")));

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    QFile file(epubPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString expectedHash = QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex());

    QCOMPARE(epub.getFileHash(), expectedHash);
    QCOMPARE(epub.getFileHash().size(), 64);
}

void EPubContainerTest::testContentHashIsStableAcrossZipEntryOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString firstPath = dir.filePath(QStringLiteral("hash-first.epub"));
    const QString secondPath = dir.filePath(QStringLiteral("hash-second.epub"));
    const QString changedPath = dir.filePath(QStringLiteral("hash-changed.epub"));
    QVERIFY(writeHashTestEpub(firstPath, false, QByteArrayLiteral("Same logical content")));
    QVERIFY(writeHashTestEpub(secondPath, true, QByteArrayLiteral("Same logical content")));
    QVERIFY(writeHashTestEpub(changedPath, true, QByteArrayLiteral("Changed logical content")));

    EPubContainer first(nullptr);
    QVERIFY(first.openFile(firstPath));
    EPubContainer second(nullptr);
    QVERIFY(second.openFile(secondPath));
    EPubContainer changed(nullptr);
    QVERIFY(changed.openFile(changedPath));

    const QString firstHash = first.getContentHash();
    QVERIFY(!firstHash.isEmpty());
    QCOMPARE(firstHash.size(), 64);
    QCOMPARE(second.getContentHash(), firstHash);
    QVERIFY(changed.getContentHash() != firstHash);
}

void EPubContainerTest::testCreateTargetAnchorIsReferenceable()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("target-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Target Anchor Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p>Hello target world</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QString anchorId = epub.createAnchor(QStringLiteral("epubcfi(/6/2!/2/2,/1:6,/1:12)"), QString());
    QVERIFY(!anchorId.isEmpty());

    const QString anchoredPath = dir.filePath(QStringLiteral("bookid.anchored.epub"));
    EPubContainer anchored(nullptr);
    QVERIFY(anchored.openFile(anchoredPath));
    anchored.extractTargetAnchors();

    const TargetAnchorInfo *anchor = anchored.targetAnchorByRef(anchorId);
    QVERIFY(anchor);
    QCOMPARE(anchor->location, QStringLiteral("OEBPS/chapter.xhtml#") + anchorId);
    QCOMPARE(anchor->previewHtml, QStringLiteral("target"));
    QCOMPARE(anchor->type, QStringLiteral("anchor"));

    KZip anchoredZip(anchoredPath);
    QVERIFY(anchoredZip.open(QIODevice::ReadOnly));
    const KArchiveFile *chapter = anchoredZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapter);

    QScopedPointer<QIODevice> device(chapter->createDevice());
    QVERIFY(device);
    const QString document = QString::fromUtf8(device->readAll());
    QVERIFY(document.contains(QStringLiteral("id=\"") + anchorId + QStringLiteral("\"")));
    QVERIFY(document.contains(QStringLiteral("data-role=\"anchor\"")));
    QVERIFY(!document.contains(QStringLiteral("data-anchor-type=\"crossref\"")));
}

void EPubContainerTest::testServerReadyEpubCanIncludeReferencesDocument()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("references-source.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Reference Include Test</dc:title></metadata><manifest><item id="chapter" href="chapter.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><p><a id="source1" title="S.B. 4.4.1" data-role="anchor" data-anchor-type="crossref">S.B. 4.4.1</a></p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    EpubReference reference;
    reference.sourceAnchorId = QStringLiteral("source1");
    reference.targetBookId = QStringLiteral("target-book");
    reference.targetAnchorId = QStringLiteral("target-anchor");
    reference.targetLocation = QStringLiteral("chapter.xhtml#target-anchor");
    reference.targetPreviewHtml = QStringLiteral("Target preview");

    ServerReadyEpubOptions options;
    options.includeReferences = true;
    options.references = {reference};

    const QByteArray data = epub.createServerReadyEpub(options);
    QVERIFY(!data.isEmpty());

    QBuffer buffer;
    buffer.setData(data);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    KZip servedZip(&buffer);
    QVERIFY(servedZip.open(QIODevice::ReadOnly));

    const KArchiveFile *packageFile = servedZip.directory()->file(QStringLiteral("OEBPS/package.opf"));
    QVERIFY(packageFile);
    QScopedPointer<QIODevice> packageDevice(packageFile->createDevice());
    QVERIFY(packageDevice);
    const QString packageDocument = QString::fromUtf8(packageDevice->readAll());
    QVERIFY(packageDocument.contains(QStringLiteral("references.xhtml")));
    QVERIFY(packageDocument.contains(QStringLiteral("idref=\"arianna-references\"")));

    const KArchiveFile *chapterFile = servedZip.directory()->file(QStringLiteral("OEBPS/chapter.xhtml"));
    QVERIFY(chapterFile);
    QScopedPointer<QIODevice> chapterDevice(chapterFile->createDevice());
    QVERIFY(chapterDevice);
    const QString chapterDocument = QString::fromUtf8(chapterDevice->readAll());
    QVERIFY(chapterDocument.contains(QStringLiteral("href=\"references.xhtml#source1\"")));

    const KArchiveFile *referencesFile = servedZip.directory()->file(QStringLiteral("OEBPS/references.xhtml"));
    QVERIFY(referencesFile);
    QScopedPointer<QIODevice> referencesDevice(referencesFile->createDevice());
    QVERIFY(referencesDevice);
    const QString referencesDocument = QString::fromUtf8(referencesDevice->readAll());
    QVERIFY(referencesDocument.contains(QStringLiteral("id=\"source1\"")));
    QVERIFY(referencesDocument.contains(QStringLiteral("target-book")));
    QVERIFY(referencesDocument.contains(QStringLiteral("Target preview")));
}

void EPubContainerTest::testReferenceableTargetsIncludeDocumentsAndElementIds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("referenceable-targets.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    writeContainer(zip);
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="bookid">bookid</dc:identifier><dc:title>Referenceable Targets Test</dc:title></metadata><manifest><item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/><item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/><item id="chapter2" href="chapter2.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter1"/><itemref idref="chapter2"/></spine></package>)"));
    zip.writeFile(QStringLiteral("OEBPS/nav.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><body><nav epub:type="toc"><ol><li><a href="chapter2.xhtml">Second Chapter</a></li></ol></nav></body></html>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter1.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><section id="section1"><h1>Section One</h1><p>First section text.</p></section><p>No id paragraph target.</p><a id="source-anchor" data-role="anchor" data-anchor-type="crossref">source</a></body></html>)"));
    zip.writeFile(QStringLiteral("OEBPS/chapter2.xhtml"), QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><body><a name="named-target">Named Target</a><p>Second chapter.</p></body></html>)"));
    zip.close();

    EPubContainer epub(nullptr);
    QVERIFY(epub.openFile(epubPath));

    const QVector<TargetAnchorInfo> targets = epub.referenceableTargets();
    auto findByLocation = [&targets](const QString &location) -> const TargetAnchorInfo * {
        for (const TargetAnchorInfo &target : targets) {
            if (target.location == location) {
                return &target;
            }
        }
        return nullptr;
    };

    const TargetAnchorInfo *chapter = findByLocation(QStringLiteral("OEBPS/chapter1.xhtml"));
    QVERIFY(chapter);
    QCOMPARE(chapter->ref, QStringLiteral("OEBPS/chapter1.xhtml"));
    QCOMPARE(chapter->type, QStringLiteral("document"));
    QVERIFY(chapter->previewHtml.contains(QStringLiteral("Section One")));

    const TargetAnchorInfo *section = findByLocation(QStringLiteral("OEBPS/chapter1.xhtml#section1"));
    QVERIFY(section);
    QCOMPARE(section->ref, QStringLiteral("OEBPS/chapter1.xhtml#section1"));
    QCOMPARE(section->type, QStringLiteral("element"));
    QVERIFY(section->previewHtml.contains(QStringLiteral("First section text")));

    QVERIFY(!findByLocation(QStringLiteral("OEBPS/chapter1.xhtml#source-anchor")));
    QVERIFY(findByLocation(QStringLiteral("OEBPS/chapter2.xhtml")));
    QVERIFY(findByLocation(QStringLiteral("OEBPS/chapter2.xhtml#named-target")));
    QVERIFY(!findByLocation(QStringLiteral("OEBPS/nav.xhtml")));

    bool foundGeneratedParagraphTarget = false;
    for (const TargetAnchorInfo &target : targets) {
        if (target.type == QStringLiteral("paragraph") && target.location.startsWith(QStringLiteral("epubcfi("))
            && target.previewHtml.contains(QStringLiteral("No id paragraph target"))) {
            foundGeneratedParagraphTarget = true;
            break;
        }
    }
    QVERIFY(foundGeneratedParagraphTarget);
}

QTEST_MAIN(EPubContainerTest)
#include "epubcontainertest.moc"
