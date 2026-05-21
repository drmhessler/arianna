// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <qqmlintegration.h>

class EditorProcess : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit EditorProcess(QObject *parent = nullptr);

    Q_INVOKABLE void start(const QString &program, const QStringList &arguments);
    Q_INVOKABLE void stop();
    Q_INVOKABLE QByteArray readAllStandardOutput();
    Q_INVOKABLE QByteArray readAllStandardError();

Q_SIGNALS:
    void processStarted();
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void processErrorOccurred(QProcess::ProcessError error);
    void editedFileChanged(const QString &file);
    void readyRead();

private Q_SLOTS:
    void handleStarted();
    void handleFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleErrorOccurred(QProcess::ProcessError error);
    void handleEditedFileChanged(const QString &file);
    void handleEditedDirectoryChanged(const QString &path);
    void handleReadyRead();

private:
    QProcess *process = nullptr;
    QFileSystemWatcher *fileWatcher = nullptr;
    QString watchedFilePath;
};
