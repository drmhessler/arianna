// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "editorprocess.h"

#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

EditorProcess::EditorProcess(QObject *parent)
    : QObject(parent)
    , process(new QProcess(this))
    , fileWatcher(new QFileSystemWatcher(this))
{
    connect(process, &QProcess::started, this, &EditorProcess::handleStarted);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &EditorProcess::handleFinished);
    connect(process, &QProcess::errorOccurred, this, &EditorProcess::handleErrorOccurred);
    connect(process, &QProcess::readyReadStandardOutput, this, &EditorProcess::handleReadyRead);
    connect(fileWatcher, &QFileSystemWatcher::fileChanged, this, &EditorProcess::handleEditedFileChanged);
    connect(fileWatcher, &QFileSystemWatcher::directoryChanged, this, &EditorProcess::handleEditedDirectoryChanged);
}

void EditorProcess::start(const QString &program, const QStringList &arguments)
{
    if (arguments.isEmpty()) {
        return;
    }

    watchedFilePath = arguments.first();
    if (!watchedFilePath.isEmpty()) {
        fileWatcher->addPath(watchedFilePath);
        fileWatcher->addPath(QFileInfo(watchedFilePath).absolutePath());
    }

    process->start(program, arguments);
}

bool EditorProcess::startDetached(const QString &program, const QStringList &arguments)
{
    if (program.isEmpty()) {
        return false;
    }

    return QProcess::startDetached(program, arguments);
}

void EditorProcess::stop()
{
    if (process->state() == QProcess::NotRunning) {
        return;
    }

    process->terminate();
    if (!process->waitForFinished(3000)) {
        process->kill();
    }
}

QByteArray EditorProcess::readAllStandardOutput()
{
    return process->readAllStandardOutput();
}

QByteArray EditorProcess::readAllStandardError()
{
    return process->readAllStandardError();
}

void EditorProcess::handleStarted()
{
    Q_EMIT processStarted();
}

void EditorProcess::handleFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    fileWatcher->removePaths(fileWatcher->files());
    fileWatcher->removePaths(fileWatcher->directories());
    Q_EMIT processFinished(exitCode, exitStatus);
}

void EditorProcess::handleErrorOccurred(QProcess::ProcessError error)
{
    Q_EMIT processErrorOccurred(error);
}

void EditorProcess::handleEditedFileChanged(const QString &file)
{
    qDebug() << "Edited file changed:" << file;
    if (QFile::exists(file) && !fileWatcher->files().contains(file)) {
        fileWatcher->addPath(file);
    }
    Q_EMIT editedFileChanged(file);
}

void EditorProcess::handleEditedDirectoryChanged(const QString &path)
{
    Q_UNUSED(path)

    if (QFile::exists(watchedFilePath)) {
        if (!fileWatcher->files().contains(watchedFilePath)) {
            fileWatcher->addPath(watchedFilePath);
            qDebug() << "Re-added file to watcher:" << watchedFilePath;
        }
        Q_EMIT editedFileChanged(watchedFilePath);
    }
}

void EditorProcess::handleReadyRead()
{
    Q_EMIT readyRead();
}

#include "moc_editorprocess.cpp"
