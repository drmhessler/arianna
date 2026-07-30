// SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL

#include "annotationstore.h"

#include "bookdatabase.h"

AnnotationStore::AnnotationStore(QObject *parent)
    : QObject(parent)
{
    connect(&BookDatabase::self(), &BookDatabase::databaseChanged, this, [this]() {
        Q_EMIT annotationsChanged(QString());
    });
}

QVariantList AnnotationStore::loadAnnotations(const QString &bookId) const
{
    return BookDatabase::self().loadAnnotations(bookId);
}

void AnnotationStore::saveAnnotation(const QString &bookId, const QVariantMap &annotation)
{
    BookDatabase::self().saveAnnotation(bookId, annotation);
    Q_EMIT annotationsChanged(bookId);
}

void AnnotationStore::removeAnnotation(const QString &bookId, const QString &value)
{
    BookDatabase::self().removeAnnotation(bookId, value);
    Q_EMIT annotationsChanged(bookId);
}

#include "moc_annotationstore.cpp"
