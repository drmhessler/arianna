// SPDX-FileCopyrightText: Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard as FormCard
import org.kde.arianna as Arianna

FormCard.FormCardPage {
    id: root

    property var metadata
    readonly property bool wideLayout: width >= Kirigami.Units.gridUnit * 36

    implicitWidth: Kirigami.Units.gridUnit * 44

    function normalizedIdentifier(identifier) {
        let value = identifier ? String(identifier).trim() : "";
        if (value.toLowerCase().startsWith("urn:uuid:")) {
            value = value.slice(9);
        }
        return value.toLowerCase();
    }

    function otherIdentifiers() {
        const uniqueIdentifier = normalizedIdentifier(root.metadata.uniqueIdentifier);
        return (root.metadata.identifier || "").split(",").map(identifier => identifier.trim()).filter(identifier => {
            return identifier.length > 0 && (!uniqueIdentifier || normalizedIdentifier(identifier) !== uniqueIdentifier);
        }).join(", ");
    }

    function coverSource() {
        const thumbnail = root.metadata.thumbnail || "";
        if (thumbnail.length === 0) {
            return "";
        }
        if (thumbnail.startsWith("file:") || thumbnail.startsWith("qrc:")) {
            return thumbnail;
        }
        return "file://" + thumbnail;
    }

    title: i18nc("@info:title", "Book Details")

    FormCard.FormHeader {
        title: root.metadata.title
    }

    GridLayout {
        columns: root.wideLayout ? 2 : 1
        columnSpacing: Kirigami.Units.gridUnit
        rowSpacing: Kirigami.Units.largeSpacing
        Layout.fillWidth: true

        Rectangle {
            id: coverFrame

            readonly property real coverWidth: root.wideLayout ? Kirigami.Units.gridUnit * 15 : Math.min(root.width - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 15)

            Layout.preferredWidth: coverFrame.coverWidth
            Layout.preferredHeight: coverFrame.coverWidth * 1.45
            Layout.alignment: root.wideLayout ? Qt.AlignTop : Qt.AlignHCenter

            radius: Kirigami.Units.cornerRadius
            color: Kirigami.Theme.alternateBackgroundColor
            border.color: Kirigami.ColorUtils.linearInterpolation(Kirigami.Theme.textColor, Kirigami.Theme.backgroundColor, 0.75)
            border.width: 1

            Image {
                id: coverImage

                anchors.fill: parent
                anchors.margins: Kirigami.Units.smallSpacing
                fillMode: Image.PreserveAspectFit
                source: root.coverSource()
                asynchronous: true

                sourceSize {
                    width: width
                    height: height
                }
            }

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Math.min(parent.width, parent.height) * 0.55
                height: width
                source: "application-epub+zip"
                visible: coverImage.status === Image.Error || coverImage.source.toString().length === 0
            }
        }

        FormCard.FormCard {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop

            FormCard.FormTextDelegate {
                id: authorField

                text: i18n("Author:")
                description: root.metadata.author.join(', ')
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: authorField.visible
            }

            FormCard.FormTextDelegate {
                id: descriptionField

                text: i18n("Description:")
                description: root.metadata.description
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: descriptionField.visible
            }

            FormCard.FormTextDelegate {
                id: publisherField

                text: i18n("Publisher:")
                description: root.metadata.publisher
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: publisherField.visible
            }

            FormCard.FormTextDelegate {
                id: languageField

                text: i18n("Language:")
                description: Qt.locale(root.metadata.language).nativeLanguageName
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: languageField.visible
            }

            FormCard.FormTextDelegate {
                id: publishingField

                text: i18n("Publishing date:")
                description: /^\d+$/.test(root.metadata.pubdate) ? root.metadata.pubdate : new Date(root.metadata.pubdate).toLocaleDateString()
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: publishingField.visible && copyrightField.visible
            }

            FormCard.FormTextDelegate {
                id: copyrightField
                text: i18n("Copyright:")
                description: root.metadata.rights
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: copyrightField.visible && locationField.visible
            }

            FormCard.FormTextDelegate {
                id: locationField
                text: i18n("Location:")
                description: root.metadata.filename
                visible: description.length > 0
                trailing: QQC2.ToolButton {
                    display: QQC2.AbstractButton.IconOnly
                    icon.name: "document-open-folder"
                    text: i18nc("@action:button", "Open Containing Folder")
                    enabled: locationField.description.length > 0
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: Arianna.FileOpener.openContainingFolder(locationField.description)
                }
            }

            FormCard.FormDelegateSeparator {
                visible: locationField.visible && uniqueIdentifierField.visible
            }

            FormCard.FormTextDelegate {
                id: uniqueIdentifierField
                text: i18n("Unique identifier:")
                description: root.metadata.uniqueIdentifier || ""
                visible: description.length > 0
            }

            FormCard.FormDelegateSeparator {
                visible: uniqueIdentifierField.visible && identifiersField.visible
            }

            FormCard.FormTextDelegate {
                id: identifiersField
                text: i18n("Further identifiers:")
                description: root.otherIdentifiers()
                visible: description.length > 0
            }
        }
    }
}
