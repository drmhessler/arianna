// SPDX-FileCopyrightText: 2026 Markus
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QVariantMap>

/**
 * Converts the XML output of veraPDF's Arlington validation model into a
 * QML-friendly report map. The original XML remains owned by the caller so
 * that it can optionally be shown for debugging.
 */
class PdfValidationReportParser
{
public:
    static QVariantMap parse(const QString &xml);
};
