// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include "bookrevision.h"

#include <QString>

#include <optional>

struct BookSnapshot {
    bool success = false;
    QString errorMessage;

    QString bookId;
    QString filename;

    QByteArray textContentHash;
    QByteArray documentStateHash;

    std::optional<BookRevision> revision;
};

struct BookCommitResult {
    bool success = false;
    bool conflict = false;

    QUuid oldRevisionId;
    QUuid newRevisionId;

    QByteArray textContentHash;
    QByteArray documentStateHash;

    std::optional<QUuid> createdAnchorId;
    QString errorMessage;
};

class BookTruthStore
{
public:
    BookSnapshot openBook(const QString &bookId) const;

    BookCommitResult createAnchor(const QString &bookId, const QString &cfiRange, const QUuid &expectedRevisionId);

    std::optional<BookRevision> currentRevision(const QString &bookId) const;
};
