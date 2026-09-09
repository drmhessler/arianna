// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#include "pdfvalidationreportparser.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QVariantList>
#include <QXmlStreamReader>

namespace
{
bool attributeIsTrue(const QXmlStreamAttributes &attributes, const QStringView &name)
{
    const QString value = attributes.value(name).toString();
    return value == QLatin1String("true") || value == QLatin1String("1");
}

int integerAttribute(const QXmlStreamAttributes &attributes, const QStringView &name)
{
    bool ok = false;
    const int value = attributes.value(name).toInt(&ok);
    return ok ? value : 0;
}

double durationInSeconds(const QString &duration)
{
    static const QRegularExpression durationPattern(QStringLiteral(R"(^(\d+):(\d{2}):(\d{2})(?:\.(\d{1,3}))?$)"));
    const QRegularExpressionMatch match = durationPattern.match(duration.trimmed());
    if (!match.hasMatch()) {
        return -1.0;
    }

    const int hours = match.captured(1).toInt();
    const int minutes = match.captured(2).toInt();
    const int seconds = match.captured(3).toInt();
    QString milliseconds = match.captured(4);
    milliseconds = milliseconds.leftJustified(3, QLatin1Char('0'));
    return hours * 3600.0 + minutes * 60.0 + seconds + milliseconds.toInt() / 1000.0;
}

QString objectNumberFromContext(const QString &context)
{
    static const QRegularExpression objectPattern(QStringLiteral(R"(\b(\d+)\s+(\d+)\s+obj\b)"));
    QRegularExpressionMatchIterator matches = objectPattern.globalMatch(context);
    QString objectNumber;
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        objectNumber = match.captured(1) + QLatin1Char(' ') + match.captured(2);
    }
    return objectNumber;
}
}

QVariantMap PdfValidationReportParser::parse(const QString &xml)
{
    QVariantMap report;
    report.insert(QStringLiteral("valid"), false);
    report.insert(QStringLiteral("fileName"), QString());
    report.insert(QStringLiteral("displayFileName"), QString());
    report.insert(QStringLiteral("profileName"), QString());
    report.insert(QStringLiteral("statement"), QString());
    report.insert(QStringLiteral("compliant"), false);
    report.insert(QStringLiteral("deviations"), 0);
    report.insert(QStringLiteral("failedRules"), 0);
    report.insert(QStringLiteral("duration"), QString());
    report.insert(QStringLiteral("durationSeconds"), -1.0);
    report.insert(QStringLiteral("rules"), QVariantList{});

    QXmlStreamReader reader(xml);
    QVariantList rules;
    QVariantMap currentRule;
    QVariantMap currentCheck;
    bool inItem = false;
    bool inReport = false;
    bool inRule = false;
    bool inCheck = false;
    bool currentJobHasReport = false;
    bool foundReport = false;

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            const QStringView element = reader.name();
            const QXmlStreamAttributes attributes = reader.attributes();

            if (element == u"job") {
                currentJobHasReport = false;
            } else if (element == u"item") {
                inItem = true;
            } else if (element == u"name" && inItem) {
                const QString fileName = reader.readElementText().trimmed();
                report.insert(QStringLiteral("fileName"), fileName);
                report.insert(QStringLiteral("displayFileName"), QFileInfo(fileName).fileName());
            } else if (element == u"arlingtonReport") {
                inReport = true;
                currentJobHasReport = true;
                foundReport = true;
                report.insert(QStringLiteral("profileName"), attributes.value(u"profileName").toString());
                report.insert(QStringLiteral("statement"), attributes.value(u"statement").toString());
                report.insert(QStringLiteral("compliant"), attributeIsTrue(attributes, u"isCompliant"));
            } else if (element == u"details" && inReport) {
                report.insert(QStringLiteral("deviations"), integerAttribute(attributes, u"deviations"));
            } else if (element == u"rule" && inReport) {
                inRule = true;
                currentRule.clear();
                currentRule.insert(QStringLiteral("specification"), attributes.value(u"specification").toString());
                currentRule.insert(QStringLiteral("clause"), attributes.value(u"clause").toString());
                currentRule.insert(QStringLiteral("testNumber"), attributes.value(u"testNumber").toString());
                currentRule.insert(QStringLiteral("deviations"), integerAttribute(attributes, u"deviations"));
                currentRule.insert(QStringLiteral("status"), attributes.value(u"status").toString());
                currentRule.insert(QStringLiteral("description"), QString());
                currentRule.insert(QStringLiteral("object"), QString());
                currentRule.insert(QStringLiteral("checks"), QVariantList{});
            } else if (element == u"description" && inRule) {
                currentRule.insert(QStringLiteral("description"), reader.readElementText().trimmed());
            } else if (element == u"object" && inRule) {
                currentRule.insert(QStringLiteral("object"), reader.readElementText().trimmed());
            } else if (element == u"check" && inRule) {
                inCheck = true;
                currentCheck.clear();
                currentCheck.insert(QStringLiteral("status"), attributes.value(u"status").toString());
                currentCheck.insert(QStringLiteral("context"), QString());
                currentCheck.insert(QStringLiteral("errorMessage"), QString());
                currentCheck.insert(QStringLiteral("pdfObjectNumber"), QString());
            } else if (element == u"context" && inCheck) {
                const QString context = reader.readElementText().trimmed();
                currentCheck.insert(QStringLiteral("context"), context);
                currentCheck.insert(QStringLiteral("pdfObjectNumber"), objectNumberFromContext(context));
            } else if (element == u"errorMessage" && inCheck) {
                currentCheck.insert(QStringLiteral("errorMessage"), reader.readElementText().trimmed());
            } else if (element == u"duration" && currentJobHasReport && report.value(QStringLiteral("duration")).toString().isEmpty()) {
                const QString duration = reader.readElementText().trimmed();
                report.insert(QStringLiteral("duration"), duration);
                report.insert(QStringLiteral("durationSeconds"), durationInSeconds(duration));
            }
        } else if (reader.isEndElement()) {
            const QStringView element = reader.name();
            if (element == u"item") {
                inItem = false;
            } else if (element == u"check" && inCheck) {
                inCheck = false;
                if (currentCheck.value(QStringLiteral("status")).toString() == QLatin1String("failed")) {
                    QVariantList checks = currentRule.value(QStringLiteral("checks")).toList();
                    checks.append(currentCheck);
                    currentRule.insert(QStringLiteral("checks"), checks);
                }
            } else if (element == u"rule" && inRule) {
                inRule = false;
                const QVariantList checks = currentRule.value(QStringLiteral("checks")).toList();
                if (currentRule.value(QStringLiteral("status")).toString() == QLatin1String("failed") || !checks.isEmpty()) {
                    rules.append(currentRule);
                }
            } else if (element == u"arlingtonReport") {
                inReport = false;
            }
        }
    }

    if (reader.hasError() || !foundReport) {
        report.insert(QStringLiteral("error"), reader.hasError() ? QStringLiteral("invalidXml") : QStringLiteral("missingArlingtonReport"));
        return report;
    }

    report.insert(QStringLiteral("valid"), true);
    report.insert(QStringLiteral("failedRules"), rules.size());
    report.insert(QStringLiteral("rules"), rules);
    return report;
}
