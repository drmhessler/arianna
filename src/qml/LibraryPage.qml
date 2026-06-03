// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtWebEngine
import QtWebChannel
import QtQuick.Layouts
import org.kde.kitemmodels as KItemModels
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components
import org.kde.quickcharts as Charts
import org.kde.arianna

Kirigami.ScrollablePage {
    id: root

    property CategoryEntriesModel bookListModel
    property var addBookAction
    property string pageTitle: i18n("Library")
    property bool showBookCount: root.bookListModel === applicationWindow().bookListModel
    readonly property int totalBookCount: applicationWindow().bookListModel ? applicationWindow().bookListModel.count : 0

    title: showBookCount ? i18nc("@title:window, %1 is the page title and %2 is the number of books", "%1 (%2)", pageTitle, totalBookCount) : pageTitle

    function canEditBook(fileName) {
        return Config.editorPath.trim().length > 0 && fileName && fileName.length > 0;
    }

    function editBook(fileName) {
        if (!canEditBook(fileName)) {
            return;
        }

        EditorProcess.start(Config.editorPath.trim(), [fileName]);
    }

    function confirmRemoveBook(fileName) {
        if (!fileName || fileName.length === 0) {
            return;
        }

        removeBookDialog.openForBook(fileName);
    }

    function escapeHtml(text) {
        return String(text)
            .replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;")
            .replace(/'/g, "&#39;");
    }

    actions: [
        Kirigami.Action {
            text: i18nc("@action:button", "Sort")
            icon.name: "view-sort"
            Kirigami.Action {
                text: i18nc("@action:inmenu Sort books by title", "Title")
                checkable: true
                checked: sortProxy.sortRoleName === "title"
                onTriggered: {
                    sortProxy.sortRoleName = "title";
                    sortProxy.sortOrder = Qt.AscendingOrder;
                }
            }
            Kirigami.Action {
                text: i18nc("@action:inmenu Sort books by author", "Author")
                checkable: true
                checked: sortProxy.sortRoleName === "authorSort"
                onTriggered: {
                    sortProxy.sortRoleName = "authorSort";
                    sortProxy.sortOrder = Qt.AscendingOrder;
                }
            }
            Kirigami.Action {
                text: i18nc("@action:inmenu Sort books by last access", "Last Access")
                checkable: true
                checked: sortProxy.sortRoleName === "lastOpenedTime"
                onTriggered: {
                    sortProxy.sortRoleName = "lastOpenedTime";
                    sortProxy.sortOrder = Qt.DescendingOrder;
                }
            }
        },
        Kirigami.Action {
            id: addBookActionProxy

            text: root.addBookAction ? root.addBookAction.text : ""
            icon.name: root.addBookAction ? root.addBookAction.icon.name : ""
            enabled: root.addBookAction ? root.addBookAction.enabled : false
            onTriggered: if (root.addBookAction) {
                root.addBookAction.trigger();
            }
        }
    ]

    KItemModels.KSortFilterProxyModel {
        id: sortProxy
        sourceModel: root.bookListModel
        sortRoleName: "lastOpenedTime"
        sortOrder: Qt.DescendingOrder
        dynamicSortFilter: true
    }

    QQC2.Dialog {
        id: removeBookDialog

        property string bookFileName: ""
        property string location: ""

        parent: QQC2.Overlay.overlay
        modal: true
        title: i18nc("@title:window", "Remove Book")
        standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel

        function openForBook(fileName) {
            bookFileName = fileName;
            location = fileName;
            deleteFileCheckBox.checked = false;
            open();
        }

        onAccepted: {
            applicationWindow().bookListModel.removeBook(bookFileName, deleteFileCheckBox.checked);
            bookFileName = "";
            location = "";
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            QQC2.Label {
                Layout.fillWidth: true
                text: i18nc("@info", "Do you really want to remove the book from the library? All saved data such as progress and annotations will be lost.")
                wrapMode: Text.Wrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing

                QQC2.CheckBox {
                    id: deleteFileCheckBox

                    Layout.alignment: Qt.AlignTop
                    checked: false
                }

                QQC2.Label {
                    Layout.fillWidth: true
                    text: i18nc("@option:check %1 is the book file location", "Also delete the book from the location:<br><b>%1</b><br>Delete it.", root.escapeHtml(removeBookDialog.location))
                    textFormat: Text.RichText
                    wrapMode: Text.WrapAnywhere

                    TapHandler {
                        onTapped: deleteFileCheckBox.checked = !deleteFileCheckBox.checked
                    }
                }
            }
        }
    }

    GridView {
        id: contentDirectoryView

        leftMargin: Kirigami.Units.smallSpacing
        rightMargin: Kirigami.Units.smallSpacing
        topMargin: Kirigami.Units.smallSpacing
        bottomMargin: Kirigami.Units.smallSpacing

        model: sortProxy

        cellWidth: {
            const viewWidth = contentDirectoryView.width - Kirigami.Units.smallSpacing * 2;
            let columns = Math.max(Math.floor(viewWidth / 170), 2);
            return Math.floor(viewWidth / columns);
        }
        cellHeight: {
            if (Kirigami.Settings.isMobile) {
                return cellWidth + Kirigami.Units.gridUnit * 2 + Kirigami.Units.largeSpacing;
            } else {
                return 170 + Kirigami.Units.gridUnit * 2 + Kirigami.Units.largeSpacing
            }
        }
        currentIndex: -1
        reuseItems: true
        activeFocusOnTab: true
        keyNavigationEnabled: true

        delegate: GridBrowserDelegate {
            id: bookDelegate

            required property string thumbnail
            required property string title
            required property string filename
            required property var author
            required property var entry
            required property var locations
            required property var currentLocation
            required property int categoryEntriesCount
            required property var categoryEntriesModel

            width: Kirigami.Settings.isMobile ? contentDirectoryView.cellWidth : 170
            height: contentDirectoryView.cellHeight

            imageUrl: categoryEntriesModel === '' ? ('file://' + thumbnail) : ''
            iconName: if (categoryEntriesModel !== '') {
                return thumbnail;
            } else if (thumbnail === '') {
                return 'application-epub+zip';
            } else {
                return '';
            }

            mainText: bookDelegate.title
            secondaryText: author ? bookDelegate.author.join(', ') : ''
            shrinkCoverOnHover: categoryEntriesModel === ""

            onClicked: if (categoryEntriesModel) {
                Navigation.openLibrary(title, categoryEntriesModel, false);
            } else {
                Navigation.openBook(filename, locations, currentLocation, entry);
            }

            ColumnLayout {
                anchors {
                    bottom: parent.bottom
                    right: parent.right
                    rightMargin: Kirigami.Units.largeSpacing
                    bottomMargin: bookDelegate.height - (bookDelegate.width - Kirigami.Units.largeSpacing)
                }
                z: 1
                spacing: Kirigami.Units.smallSpacing
                visible: !Kirigami.Settings.isMobile && bookDelegate.hovered && bookDelegate.categoryEntriesModel === ""

                QQC2.ToolButton {
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                    display: QQC2.AbstractButton.IconOnly
                    icon.name: "documentinfo-symbolic"
                    text: i18nc("@action:button", "Book Details")
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                        metadata: bookDelegate.entry
                    })
                }

                QQC2.ToolButton {
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                    display: QQC2.AbstractButton.IconOnly
                    icon.name: "document-edit"
                    text: i18nc("@action:button", "Edit Book")
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    enabled: root.canEditBook(bookDelegate.filename)
                    onClicked: root.editBook(bookDelegate.filename)
                }

                QQC2.ToolButton {
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                    display: QQC2.AbstractButton.IconOnly
                    icon.name: "edit-delete-remove"
                    text: i18nc("@action:button", "Remove from Library")
                    QQC2.ToolTip.text: text
                    QQC2.ToolTip.visible: hovered
                    QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: root.confirmRemoveBook(bookDelegate.filename)
                }
            }

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: {
                    menu.entry = bookDelegate.entry;
                    menu.filename = bookDelegate.filename;
                    menu.isBook = bookDelegate.categoryEntriesModel === "";
                    menu.popup();
                }
            }
        }

        Components.ConvergentContextMenu {
            id: menu

            property var entry: null
            property string filename: ""
            property bool isBook: false

            QQC2.Action {
                icon.name: 'documentinfo-symbolic'
                text: i18nc("@action:inmenu", "Book Details")
                onTriggered: applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                    metadata: menu.entry,
                })
            }

            QQC2.Action {
                icon.name: 'document-edit'
                text: i18nc("@action:inmenu", "Edit Book")
                enabled: menu.isBook && root.canEditBook(menu.filename)
                onTriggered: root.editBook(menu.filename)
            }

            QQC2.Action {
                icon.name: 'edit-delete-remove'
                text: i18nc("@action:inmenu", "Remove from Library")
                enabled: menu.isBook
                onTriggered: root.confirmRemoveBook(menu.filename)
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - (Kirigami.Units.largeSpacing * 4)
            visible: contentDirectoryView.count === 0
            icon.name: "application-epub+zip"
            text: i18nc("@info placeholder", "Add some books")
            helpfulAction: addBookActionProxy
        }
    }
}
