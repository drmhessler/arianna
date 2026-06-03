// SPDX-FileCopyrightText: 2026 Arianna contributors
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "epubcontainer.h"

#include <KZip>

#include <QIODevice>
#include <QObject>
#include <QTemporaryDir>
#include <QTest>

class EPubContainerTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testUniqueIdentifierMetadata();
};

void EPubContainerTest::testUniqueIdentifierMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QString epubPath = dir.filePath(QStringLiteral("book.epub"));
    KZip zip(epubPath);
    QVERIFY(zip.open(QIODevice::WriteOnly));
    zip.writeFile(QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip"));
    zip.writeFile(QStringLiteral("META-INF/container.xml"), QByteArrayLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/package.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>)"));
    zip.writeFile(QStringLiteral("OEBPS/package.opf"), QByteArrayLiteral(R"(<?xml version="1.0" encoding="utf-8"?>
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

QTEST_MAIN(EPubContainerTest)
#include "epubcontainertest.moc"
