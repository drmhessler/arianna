// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>

#include <optional>

struct BookRevision {
    QUuid revisionId;
    QString bookId;
    std::optional<QUuid> parentRevisionId;

    QByteArray textContentHash;
    QByteArray documentStateHash;

    QString changeType;
    std::optional<QUuid> changedObjectId;
};
