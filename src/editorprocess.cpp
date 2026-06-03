// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "editorprocess.h"

#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>

EditorProcess::EditorProcess(QObject *parent)
    : QObject(parent)
    , process(new QProcess(this))
    , fileWatcher(new QFileSystemWatcher(this))
    , editedFileChangedTimer(new QTimer(this))
{
    editedFileChangedTimer->setSingleShot(true);
    editedFileChangedTimer->setInterval(500);

    connect(process, &QProcess::started, this, &EditorProcess::handleStarted);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &EditorProcess::handleFinished);
    connect(process, &QProcess::errorOccurred, this, &EditorProcess::handleErrorOccurred);
    connect(process, &QProcess::readyReadStandardOutput, this, &EditorProcess::handleReadyRead);
    connect(fileWatcher, &QFileSystemWatcher::fileChanged, this, &EditorProcess::handleEditedFileChanged);
    connect(fileWatcher, &QFileSystemWatcher::directoryChanged, this, &EditorProcess::handleEditedDirectoryChanged);
    connect(editedFileChangedTimer, &QTimer::timeout, this, [this] {
        const QString file = pendingEditedFile;
        pendingEditedFile.clear();

        if (!file.isEmpty()) {
            Q_EMIT editedFileChanged(file);
        }
    });
}

void EditorProcess::start(const QString &program, const QStringList &arguments)
{
    if (arguments.isEmpty()) {
        return;
    }

    watchFile(arguments.first());

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

void EditorProcess::clearWatchedFile()
{
    editedFileChangedTimer->stop();
    pendingEditedFile.clear();
    watchedFilePath.clear();
    watchedFileLastModified = {};
    watchedFileSize = -1;

    if (!fileWatcher->files().isEmpty()) {
        fileWatcher->removePaths(fileWatcher->files());
    }

    if (!fileWatcher->directories().isEmpty()) {
        fileWatcher->removePaths(fileWatcher->directories());
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

    if (QFileInfo(file).absoluteFilePath() != watchedFilePath) {
        return;
    }

    updateWatchedFileState();
    scheduleEditedFileChanged(file);
}

void EditorProcess::handleEditedDirectoryChanged(const QString &path)
{
    Q_UNUSED(path)

    if (watchedFilePath.isEmpty() || !QFile::exists(watchedFilePath)) {
        return;
    }

    if (!fileWatcher->files().contains(watchedFilePath)) {
        fileWatcher->addPath(watchedFilePath);
        qDebug() << "Re-added file to watcher:" << watchedFilePath;
    }

    if (updateWatchedFileState()) {
        scheduleEditedFileChanged(watchedFilePath);
    }
}

void EditorProcess::handleReadyRead()
{
    Q_EMIT readyRead();
}

void EditorProcess::watchFile(const QString &filePath)
{
    const QFileInfo fileInfo(filePath);
    const QString absoluteFilePath = fileInfo.absoluteFilePath();

    if (absoluteFilePath.isEmpty()) {
        return;
    }

    if (absoluteFilePath != watchedFilePath) {
        clearWatchedFile();
    }

    watchedFilePath = absoluteFilePath;
    updateWatchedFileState();

    if (QFile::exists(watchedFilePath) && !fileWatcher->files().contains(watchedFilePath)) {
        fileWatcher->addPath(watchedFilePath);
    }

    const QString directory = QFileInfo(watchedFilePath).absolutePath();
    if (!directory.isEmpty() && !fileWatcher->directories().contains(directory)) {
        fileWatcher->addPath(directory);
    }
}

void EditorProcess::scheduleEditedFileChanged(const QString &filePath)
{
    pendingEditedFile = QFileInfo(filePath).absoluteFilePath();
    editedFileChangedTimer->start();
}

bool EditorProcess::updateWatchedFileState()
{
    const QFileInfo fileInfo(watchedFilePath);

    if (!fileInfo.exists()) {
        const bool changed = watchedFileSize != -1 || watchedFileLastModified.isValid();
        watchedFileSize = -1;
        watchedFileLastModified = {};
        return changed;
    }

    const QDateTime lastModified = fileInfo.lastModified();
    const qint64 size = fileInfo.size();
    const bool changed = watchedFileLastModified.isValid() && (watchedFileLastModified != lastModified || watchedFileSize != size);

    watchedFileLastModified = lastModified;
    watchedFileSize = size;

    return changed;
}

#include "moc_editorprocess.cpp"
