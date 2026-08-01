// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Window
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import Qt.labs.platform
import org.kde.arianna
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.config as KConfig

Kirigami.ApplicationWindow {
    id: root

    property bool isLoading: true
    property string librarySearchText: ""
    property bool readerFullScreen: false
    readonly property bool directStartupReader: typeof startupDirectReaderMode !== "undefined" && startupDirectReaderMode === true

    title: i18n("Arianna")

    function centerToolbarActionsFor(page) {
        if (!page) {
            return false;
        }

        try {
            return page.centerToolbarActions === true;
        } catch (error) {
            return false;
        }
    }

    function toggleReaderFullScreen() {
        root.readerFullScreen = !root.readerFullScreen;
    }

    onReaderFullScreenChanged: {
        visibility = readerFullScreen ? Window.FullScreen : Window.Windowed;
    }

    function localPathFromUrl(url) {
        const value = String(url);
        if (value.startsWith("file://")) {
            return decodeURIComponent(value.replace("file://", ""));
        }

        return value;
    }

    function fileTitleFromPath(fileName) {
        const fileTitle = String(fileName).split("/").pop();
        const extensionIndex = fileTitle.lastIndexOf(".");
        return extensionIndex > 0 ? fileTitle.substring(0, extensionIndex) : fileTitle;
    }

    function readOnlyEntryForFile(fileName, identifier) {
        return {
            filename: fileName,
            filetitle: String(fileName).split("/").pop(),
            title: fileTitleFromPath(fileName),
            uniqueIdentifier: identifier,
            zoomLevel: 1.0
        };
    }

    function registerReadOnlyBook(fileName, callback) {
        const request = new XMLHttpRequest();
        request.open("POST", "http://127.0.0.1:" + bookServerPort + "/read-only-books");
        request.setRequestHeader("Content-Type", "application/json");
        request.setRequestHeader("X-Arianna-Session-Token", bookServerSessionToken);
        request.onreadystatechange = function() {
            if (request.readyState !== 4) {
                return;
            }

            if (request.status < 200 || request.status >= 300) {
                console.warn("Unable to register read-only book", fileName, request.status, request.responseText);
                callback("");
                return;
            }

            const response = JSON.parse(request.responseText);
            callback(response.identifier || "");
        };
        request.send(JSON.stringify({
            fileName: fileName
        }));
    }

    function openReadOnlyBook(fileUrl, callback) {
        const fileName = localPathFromUrl(fileUrl);
        if (!fileName) {
            if (callback) {
                callback();
            }
            return;
        }

        registerReadOnlyBook(fileName, identifier => {
            if (identifier) {
                openBookPage(fileName, "", "", readOnlyEntryForFile(fileName, identifier), true);
            }

            if (callback) {
                callback();
            }
        });
    }

    function openReadOnlyBooks(fileUrls) {
        let index = 0;
        const openNext = () => {
            if (index >= fileUrls.length) {
                return;
            }

            const fileUrl = fileUrls[index];
            index += 1;
            openReadOnlyBook(fileUrl, openNext);
        };

        openNext();
    }

    function importBookEntry(filename) {
        contentList.addFiles(['file://' + filename]);

        const importedEntry = bookListModel.bookEntryFromFile(filename);
        if (importedEntry && importedEntry.filename) {
            return importedEntry;
        }

        return null;
    }

    function openBookPage(filename, locations, currentLocation, entry, readOnly, sourceTitle = "") {
        let bookEntry = entry;
        let bookLocations = locations;
        let bookCurrentLocation = currentLocation;
        let bookZoomLevel = bookEntry && bookEntry.zoomLevel > 0 ? bookEntry.zoomLevel : 1.0;
        const openReadOnly = readOnly === true;
        let latestLocation = bookCurrentLocation;
        let latestProgress = 0;
        let lastOpenedTimeUpdated = false;

        if (filename && !openReadOnly && (!bookEntry || !bookEntry.filename)) {
            const importedEntry = importBookEntry(filename);
            if (importedEntry) {
                bookEntry = importedEntry;
                bookLocations = importedEntry.locations;
                bookCurrentLocation = importedEntry.currentLocation;
                bookZoomLevel = importedEntry.zoomLevel > 0 ? importedEntry.zoomLevel : 1.0;
            }
        }

        if (filename && !openReadOnly && bookEntry && bookEntry.filename && !bookEntry.uniqueIdentifier && !bookEntry.identifier) {
            const refreshedEntry = bookListModel.refreshBookFromFile(filename, false);
            if (refreshedEntry && refreshedEntry.filename) {
                bookEntry = refreshedEntry;
                bookLocations = bookLocations || refreshedEntry.locations;
                bookCurrentLocation = bookCurrentLocation || refreshedEntry.currentLocation;
                bookZoomLevel = refreshedEntry.zoomLevel > 0 ? refreshedEntry.zoomLevel : bookZoomLevel;
            }
        }

        Navigation.ensureWebEngineInitialized();

        const epubViewerLayer = root.pageStack.layers.push('./EpubViewerPage.qml', {
            currentLocation: bookCurrentLocation,
            locations: bookLocations,
            zoomLevel: bookZoomLevel,
            filename: filename,
            entry: bookEntry,
            readOnly: openReadOnly,
            referenceSourceTitle: sourceTitle,
            // url change is a loading trigger so put this last
            url: 'file://' + filename
        });
        const epubViewer = epubViewerLayer && epubViewerLayer.item ? epubViewerLayer.item : epubViewerLayer;
        let saveLocationTimer = null;

        function ensureSaveLocationTimer() {
            if (saveLocationTimer) {
                return saveLocationTimer;
            }

            saveLocationTimer = Qt.createQmlObject('import QtQuick\nTimer { interval: 1000; repeat: false }', root, 'readerLocationSaveTimer');
            saveLocationTimer.triggered.connect(flushReadingPosition);
            return saveLocationTimer;
        }

        function flushReadingPosition() {
            if (!epubViewer || epubViewer.readOnly || !latestLocation) {
                return;
            }

            bookListModel.setBookData(filename, 'currentLocation', latestLocation);
            bookListModel.setBookData(filename, 'currentProgress', latestProgress);
        }

        function scheduleReadingPositionSave() {
            if (!epubViewer || epubViewer.readOnly) {
                return;
            }

            ensureSaveLocationTimer().restart();
        }

        function destroySaveLocationTimer() {
            if (!saveLocationTimer) {
                return;
            }

            saveLocationTimer.stop();
            saveLocationTimer.destroy();
            saveLocationTimer = null;
        }

        function updateLastOpenedTime() {
            if (lastOpenedTimeUpdated || !epubViewer || epubViewer.readOnly) {
                return;
            }

            lastOpenedTimeUpdated = true;
            bookListModel.setBookData(filename, 'lastOpenedTime', new Date().toISOString());
        }

        function connectEpubViewerSignals(viewer) {
            if (!viewer || typeof viewer.relocated === 'undefined') {
                return;
            }

            viewer.relocated.connect((newLocation, newProgress) => {
                latestLocation = newLocation;
                latestProgress = newProgress;
                scheduleReadingPositionSave();
            });

            viewer.locationsLoaded.connect(locations => {
                if (!viewer.readOnly) {
                    bookListModel.setBookData(filename, 'locations', locations);
                }
            });

            viewer.zoomLevelSaved.connect(zoomLevel => {
                if (!viewer.readOnly) {
                    bookListModel.setBookData(filename, 'zoomLevel', zoomLevel);
                }
            });

            viewer.addToLibraryRequested.connect(() => {
                const importedEntry = importBookEntry(filename);
                if (!importedEntry) {
                    return;
                }

                viewer.entry = importedEntry;
                viewer.readOnly = false;
                viewer.locations = importedEntry.locations;

                if (!viewer.currentLocation && importedEntry.currentLocation) {
                    viewer.currentLocation = importedEntry.currentLocation;
                }

                updateLastOpenedTime();
                if (latestLocation) {
                    bookListModel.setBookData(filename, 'currentLocation', latestLocation);
                }
                if (latestProgress > 0) {
                    bookListModel.setBookData(filename, 'currentProgress', latestProgress);
                }
                if (viewer.locations) {
                    bookListModel.setBookData(filename, 'locations', viewer.locations);
                }
                if (viewer.zoomLevel > 0) {
                    bookListModel.setBookData(filename, 'zoomLevel', viewer.zoomLevel);
                }
            });

            viewer.bookReady.connect(title => {
                root.title = title;
                updateLastOpenedTime();
            });

            viewer.bookClosed.connect(() => {
                flushReadingPosition();
                destroySaveLocationTimer();
                root.title = i18n("Arianna");
                root.ensureLibraryPageVisible();
            });
        }

        if (!epubViewer || typeof epubViewer.relocated === 'undefined') {
            console.error('EpubViewer page is not available yet; deferring signal connection', epubViewerLayer);
            Qt.callLater(() => {
                const deferredViewer = epubViewerLayer && epubViewerLayer.item ? epubViewerLayer.item : epubViewerLayer;
                if (deferredViewer && typeof deferredViewer.relocated !== 'undefined') {
                    connectEpubViewerSignals(deferredViewer);
                } else {
                    console.error('EpubViewer still not ready after deferral', epubViewerLayer);
                }
            });
            return;
        }

        connectEpubViewerSignals(epubViewer);
    }

    function openReferencePage(filename, currentLocation, entry, readOnly, locations, sourceTitle) {
        openBookPage(filename, locations, currentLocation, entry, readOnly, sourceTitle);
    }

    function showHomeLibraryPage() {
        root.readerFullScreen = false;
        root.pageStack.replace(Qt.resolvedUrl('./LibraryPage.qml'), {
            pageTitle: i18n("Home"),
            bookListModel: root.bookListModel,
            addBookAction: addBookAction
        });
    }

    function ensureLibraryPageVisible() {
        const currentPage = root.pageStack.currentItem;
        if (currentPage && currentPage.startupPlaceholder === true) {
            root.showHomeLibraryPage();
        }
    }

    width: Kirigami.Units.gridUnit * 65
    height: Kirigami.Units.gridUnit * 45
    minimumWidth: Kirigami.Units.gridUnit * 20
    minimumHeight: Kirigami.Units.gridUnit * 30

    Component {
        id: startupPlaceholderPage

        Kirigami.Page {
            readonly property bool startupPlaceholder: true
            readonly property bool hideSidebar: true
            readonly property bool centerToolbarActions: true
        }
    }

    Component {
        id: libraryInitialPage

        LibraryPage {
            bookListModel: root.bookListModel
            addBookAction: addBookAction
        }
    }

    pageStack {
        defaultColumnWidth: Kirigami.Units.gridUnit * 30
        globalToolBar {
            canContainHandles: true
            style: Kirigami.ApplicationHeaderStyle.ToolBar
            showNavigationButtons: applicationWindow().pageStack.currentIndex > 0 ? Kirigami.ApplicationHeaderStyle.ShowBackButton : 0
            toolbarActionAlignment: {
                const currentPage = root.pageStack.layers.depth > 1 ? root.pageStack.layers.currentItem : root.pageStack.currentItem;
                return root.centerToolbarActionsFor(currentPage) ? Qt.AlignHCenter : Qt.AlignRight;
            }
        }
        initialPage: root.directStartupReader ? startupPlaceholderPage : libraryInitialPage
    }

    readonly property BookListModel bookListModel: BookListModel {
        contentModel: ContentList {
            id: contentList

            autoSearch: false

            onSearchStarted: root.isLoading = true
            onSearchCompleted: root.isLoading = false

            ContentQuery {
                type: ContentQuery.Epub
                locations: Config.bookLocations
            }
        }
        onCacheLoadedChanged: {
            if (!cacheLoaded) {
                return;
            }
            contentModel.setKnownFiles(knownBookFiles());
            contentModel.startSearch();
        }
    }

    globalDrawer: Kirigami.OverlayDrawer {
        edge: Qt.application.layoutDirection === Qt.RightToLeft ? Qt.RightEdge : Qt.LeftEdge
        modal: Kirigami.Settings.isMobile || (applicationWindow().width < Kirigami.Units.gridUnit * 50 && !collapsed) // Only modal when not collapsed, otherwise collapsed won't show.
        z: modal ? Math.round(position * 10000000) : 100
        drawerOpen: !Kirigami.Settings.isMobile && enabled
        enabled: !root.readerFullScreen && pageStack.currentItem && pageStack.currentItem.hideSidebar !== true && (!pageStack.layers.currentItem || pageStack.layers.currentItem.hideSidebar !== true)
        onEnabledChanged: drawerOpen = !Kirigami.Settings.isMobile && enabled
        width: Kirigami.Units.gridUnit * 16
        Behavior on width {
            NumberAnimation {
                duration: Kirigami.Units.longDuration
                easing.type: Easing.InOutQuad
            }
        }
        Kirigami.Theme.colorSet: Kirigami.Theme.View
        Kirigami.Theme.inherit: false

        handleClosedIcon.source: modal ? null : "sidebar-expand-left"
        handleOpenIcon.source: modal ? null : "sidebar-collapse-left"
        handleVisible: modal
        onModalChanged: if (!modal && (!pageStack.layers.currentItem || pageStack.layers.currentItem.hideSidebar !== true)) {
            drawerOpen = true;
        }

        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0

        contentItem: ColumnLayout {
            spacing: 0

            QQC2.ToolBar {
                Layout.fillWidth: true
                Layout.preferredHeight: root.pageStack.globalToolBar.preferredHeight

                leftPadding: Kirigami.Units.smallSpacing
                rightPadding: Kirigami.Units.smallSpacing
                topPadding: Kirigami.Units.smallSpacing
                bottomPadding: Kirigami.Units.smallSpacing

                contentItem: Kirigami.SearchField {
                    text: root.librarySearchText
                    onTextChanged: root.librarySearchText = text
                }
            }

            QQC2.ButtonGroup {
                id: placeGroup
            }

            QQC2.ScrollView {
                id: scrollView

                Layout.fillHeight: true
                Layout.fillWidth: true

                contentWidth: availableWidth
                topPadding: Kirigami.Units.smallSpacing / 2

                ColumnLayout {
                    spacing: 0
                    width: scrollView.width
                    PlaceItem {
                        id: goHomeButton
                        text: i18nc("Switch to the listing page showing the most recently read books, %1 is the number of books", "Home (%1)", root.bookListModel.count)
                        icon.name: "go-home"
                        checked: true
                        QQC2.ButtonGroup.group: placeGroup
                        onTriggered: Navigation.openLibrary(i18n("Home"), bookListModel, true)
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing the most recently discovered books", "Recently Added Books")
                        icon.name: "appointment-new"
                        QQC2.ButtonGroup.group: placeGroup
                        onTriggered: Navigation.openLibrary(text, bookListModel.newlyAddedCategoryModel, true)
                    }
                    PlaceItem {
                        text: i18nc("Open a book from somewhere on disk (uses the open dialog, or a drilldown on touch devices)", "Open Other...")
                        icon.name: "document-open"
                        action: openOtherAction
                        QQC2.ButtonGroup.group: null
                        checkable: false
                    }

                    PlaceItem {
                        text: i18nc("Open the settings page", "Settings")
                        icon.name: "configure"
                        onClicked: Navigation.openSettings()
                        QQC2.ButtonGroup.group: placeGroup
                        checkable: false
                        Layout.bottomMargin: Kirigami.Units.smallSpacing / 2
                    }

                    Kirigami.ListSectionHeader {
                        text: i18nc("Heading for switching to listing page showing items grouped by some properties", "Group By")
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by author", "Author")
                        icon.name: "actor"
                        onTriggered: Navigation.openLibrary(text, bookListModel.authorCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by series", "Series")
                        icon.name: "edit-group"
                        onTriggered: Navigation.openLibrary(i18nc("Title of the page with books grouped by what series they are in", "Group by Series"), bookListModel.seriesCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by publisher", "Publisher")
                        icon.name: "view-media-publisher"
                        onTriggered: Navigation.openLibrary(text, bookListModel.publisherCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                    PlaceItem {
                        text: i18nc("Switch to the listing page showing items grouped by genres", "Keywords")
                        icon.name: "tag"
                        onTriggered: Navigation.openLibrary(i18nc("Title of the page with books grouped by genres", "Group by Genres"), bookListModel.keywordCategoryModel, true)
                        QQC2.ButtonGroup.group: placeGroup
                    }
                }
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }

    component PlaceItem: Delegates.RoundedItemDelegate {
        id: item
        signal triggered
        checkable: true
        Layout.fillWidth: true
        Keys.onDownPressed: nextItemInFocusChain().forceActiveFocus(Qt.TabFocusReason)
        Keys.onUpPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocusReason)
        Accessible.role: Accessible.MenuItem
        highlighted: checked || activeFocus
        onToggled: if (checked) {
            item.triggered();
        }
    }

    contextDrawer: TableOfContentDrawer {
        modal: !root.wideScreen || !enabled
        onEnabledChanged: drawerOpen = enabled && !modal
        enabled: root.pageStack.layers.depth > 1
        handleVisible: enabled
    }

    Connections {
        target: Navigation

        function onOpenBook(filename, locations, currentLocation, entry, readOnly) {
            root.openBookPage(filename, locations, currentLocation, entry, readOnly);
        }

        function onOpenLibrary(title, model, replace) {
            root.readerFullScreen = false;

            if (!root.pageStack.currentItem.bookListModel) {
                root.pageStack.replace(Qt.resolvedUrl('./LibraryPage.qml'), {
                    pageTitle: title,
                    bookListModel: model,
                    addBookAction: addBookAction
                });
                root.pageStack.currentItem.pageTitle = title;
                root.pageStack.currentItem.bookListModel = model;
                return;
            }
            if (replace) {
                while (root.pageStack.depth > 1) {
                    root.pageStack.pop();
                }
                root.pageStack.currentItem.pageTitle = title;
                root.pageStack.currentItem.bookListModel = model;
                return;
            }

            root.pageStack.push(Qt.resolvedUrl('./LibraryPage.qml'), {
                pageTitle: title,
                bookListModel: model,
                addBookAction: addBookAction
            });
        }

        function onOpenSettings() {
            pageStack.pushDialogLayer(Qt.resolvedUrl('./SettingsPage.qml'), {}, {
                title: i18n("Settings"),
                width: Kirigami.Units.gridUnit * 24
            });
        }
    }

    FileDialog {
        id: addBooksDialog

        title: i18n("Add Books")
        fileMode: FileDialog.OpenFiles
        nameFilters: ["eBook files (*.epub *.cb* *.fb2 *.fb2zip)"]

        onAccepted: {
            const selectedBookFiles = files.length > 0 ? files : (file ? [file] : []);
            contentList.addFiles(selectedBookFiles);
        }
    }

    FileDialog {
        id: openOtherBooksDialog

        title: i18n("Open Other...")
        fileMode: FileDialog.OpenFiles
        nameFilters: ["eBook files (*.epub *.cb* *.fb2 *.fb2zip)"]

        onAccepted: {
            const selectedBookFiles = files.length > 0 ? files : (file ? [file] : []);
            root.openReadOnlyBooks(selectedBookFiles);
        }
    }

    Kirigami.Action {
        id: addBookAction
        text: i18nc("@action:button", "Add Books…")
        icon.name: "list-add"
        onTriggered: addBooksDialog.open()
    }

    Kirigami.Action {
        id: openOtherAction
        text: i18nc("@action:button", "Open Other...")
        icon.name: "document-open"
        onTriggered: openOtherBooksDialog.open()
    }

    KConfig.WindowStateSaver {
        configGroupName: "Main"
    }
}
