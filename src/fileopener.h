// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#pragma once

#include <QObject>
#include <QString>
#include <qqmlintegration.h>

class FileOpener : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit FileOpener(QObject *parent = nullptr);

    Q_INVOKABLE bool openContainingFolder(const QString &fileName);
};
