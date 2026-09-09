// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "pdfvalidationreportparser.h"

#include <QFile>
#include <QTest>

class PdfValidationReportParserTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesArlingtonReportFixture();
    void rejectsInvalidXml();
};

void PdfValidationReportParserTest::parsesArlingtonReportFixture()
{
    QFile input(QStringLiteral(DATA_DIR "/arlington-report.xml"));
    QVERIFY(input.open(QIODevice::ReadOnly));

    const QVariantMap report = PdfValidationReportParser::parse(QString::fromUtf8(input.readAll()));
    QVERIFY(report.value(QStringLiteral("valid")).toBool());
    QCOMPARE(report.value(QStringLiteral("displayFileName")).toString(), QStringLiteral("test2_backup.pdf"));
    QCOMPARE(report.value(QStringLiteral("profileName")).toString(), QStringLiteral("Arlington PDF 1.4 profile"));
    QVERIFY(!report.value(QStringLiteral("compliant")).toBool());
    QCOMPARE(report.value(QStringLiteral("deviations")).toInt(), 5);
    QCOMPARE(report.value(QStringLiteral("failedRules")).toInt(), 3);
    QCOMPARE(report.value(QStringLiteral("duration")).toString(), QStringLiteral("00:00:00.539"));
    QVERIFY(qAbs(report.value(QStringLiteral("durationSeconds")).toDouble() - 0.539) < 0.0001);

    const QVariantList rules = report.value(QStringLiteral("rules")).toList();
    QCOMPARE(rules.size(), 3);
    const QVariantMap fontType0 = rules.constFirst().toMap();
    QCOMPARE(fontType0.value(QStringLiteral("specification")).toString(), QStringLiteral("PDF Reference 1.4"));
    QCOMPARE(fontType0.value(QStringLiteral("clause")).toString(), QStringLiteral("FontType0"));
    QCOMPARE(fontType0.value(QStringLiteral("testNumber")).toString(), QStringLiteral("1"));
    QCOMPARE(fontType0.value(QStringLiteral("object")).toString(), QStringLiteral("AFontType0"));
    QCOMPARE(fontType0.value(QStringLiteral("deviations")).toInt(), 2);

    const QVariantList checks = fontType0.value(QStringLiteral("checks")).toList();
    QCOMPARE(checks.size(), 2);
    const QVariantMap firstCheck = checks.constFirst().toMap();
    QCOMPARE(firstCheck.value(QStringLiteral("pdfObjectNumber")).toString(), QStringLiteral("56 0"));
    QCOMPARE(firstCheck.value(QStringLiteral("errorMessage")).toString(), QStringLiteral("FontType0 contains entry(ies) Name"));
    QVERIFY(firstCheck.value(QStringLiteral("context")).toString().startsWith(QStringLiteral("root/FileTrailer")));
}

void PdfValidationReportParserTest::rejectsInvalidXml()
{
    const QVariantMap report = PdfValidationReportParser::parse(QStringLiteral("<report><arlingtonReport>"));
    QVERIFY(!report.value(QStringLiteral("valid")).toBool());
    QCOMPARE(report.value(QStringLiteral("error")).toString(), QStringLiteral("invalidXml"));
}

QTEST_GUILESS_MAIN(PdfValidationReportParserTest)

#include "pdfvalidationreportparsertest.moc"
