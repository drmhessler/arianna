// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "fileopener.h"

#include "arianna_debug.h"

#include <KIO/OpenFileManagerWindowJob>
#include <KJob>

#include <QFileInfo>
#include <QUrl>

FileOpener::FileOpener(QObject *parent)
    : QObject(parent)
{
}

bool FileOpener::openContainingFolder(const QString &fileName)
{
    const QUrl fileUrl(fileName);
    const QString localFileName = fileUrl.isLocalFile() ? fileUrl.toLocalFile() : fileName;
    const QFileInfo fileInfo(localFileName);
    if (!fileInfo.exists()) {
        qCWarning(ARIANNA_LOG) << "Cannot open containing folder for missing file" << fileName;
        return false;
    }

    auto *job = KIO::highlightInFileManager({QUrl::fromLocalFile(fileInfo.absoluteFilePath())});
    connect(job, &KJob::result, this, [](KJob *finishedJob) {
        if (finishedJob->error() != KJob::NoError) {
            qCWarning(ARIANNA_LOG) << "Could not open containing folder" << finishedJob->errorString();
        }
    });

    return true;
}

#include "moc_fileopener.cpp"
