// SPDX-FileCopyrightText: 2022 Carl Schwan <carl@carlschwan.eu>
// SPDX-License-Identifier: LGPL-2.1-only or LGPL-3.0-only or LicenseRef-KDE-Accepted-LGPL

import QtQuick
import QtQuick.Controls as QQC2
import QtWebEngine
import QtWebChannel
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.arianna
import Qt.labs.platform

Kirigami.Page {
    id: root

    property string url
    property string filename
    property string locations
    property string currentLocation
    property real zoomLevel: 1.0
    property var reloadLocation: null
    property string identifier
    property var entry: null
    readonly property color readerTheme: Kirigami.Theme.backgroundColor
    readonly property bool hideSidebar: true

    property var layouts: {
        'auto': {
            'renderTo': "'viewer'",
            'options': {
                width: '100%',
                flow: 'paginated',
                maxSpreadColumns: 2
            }
        },
        'single': {
            renderTo: "'viewer'",
            options: {
                width: '100%',
                flow: 'paginated',
                spread: 'none'
            }
        },
        'scrolled': {
            renderTo: 'document.body',
            options: {
                width: '100%',
                flow: 'scrolled-doc'
            }
        },
        'continuous': {
            renderTo: 'document.body',
            options: {
                width: '100%',
                flow: 'scrolled',
                manager: 'continuous'
            }
        }
    }

    signal relocated(newLocation: var, newProgress: int)
    signal locationsLoaded(locations: var)
    signal zoomLevelSaved(zoomLevel: real)
    signal bookReady(title: var)
    signal bookClosed

    function reloadBook() {
        if (!root.url || view.loading || !view.readerReady || !root.entry) {
            return;
        }
        // HACK: renderTo and options are the value of layouts.auto, but referencing layouts.auto here crashes
        const renderTo = "'viewer'";
        const options = JSON.stringify({
            width: '100%',
            flow: 'paginated',
            maxSpreadColumns: 2
        });

        const bookUrl = root.entry && root.entry.identifier ? "http://127.0.0.1:45961/" + encodeURIComponent(root.entry.identifier) + "/book.epub" : "http://127.0.0.1:45961/book?url=" + encodeURIComponent(root.url);
        const urlNormalized = JSON.stringify(bookUrl);
        console.info("opening book", urlNormalized, renderTo, options);
        const initLocation = reloadLocation ? reloadLocation : currentLocation;
        const initCfi = initLocation ? JSON.stringify(initLocation) : "null";
        reloadLocation = null;
        view.bookReady = false;
        view.runJavaScript(`openSync(${urlNormalized}, ${initCfi})`);
    }

    function reloadCurrentBook() {
        if (backend.location && backend.location.cfi) {
            root.currentLocation = backend.location.cfi;
            root.reloadLocation = backend.location.cfi;
        }

        if (view.loading) {
            reloadChangedBookTimer.restart();
            return;
        }

        view.reload();
    }

    title: backend.metadata ? backend.metadata.title : ''
    padding: 0

    onUrlChanged: reloadBook()
    onReaderThemeChanged: backend.applyStyle()

    Connections {
        target: Config
        function onInvertChanged() {
            backend.applyStyle();
        }
        function onKdeThemingChanged() {
            backend.applyStyle();
        }
        function onReaderBackgroundPathChanged() {
            view.reload();
        }
    }

    Kirigami.SearchDialog {
        id: searchDialog

        onAccepted: if (text === '') {
            view.runJavaScript(`find.clearHighlight()`);
            searchResultModel.clear();
        } else {
            searchResultModel.search(text);
            searchResultModel.loading = true;
        }

        emptyText: if (searchResultModel.loading) {
            return i18n("Loading");
        } else if (count === 0 && searchDialog.text.length > 2) {
            return i18n("No search results");
        } else {
            return '';
        }

        model: searchResultModel

        delegate: Delegates.RoundedItemDelegate {
            id: searchDelegate

            required property string sectionMarkup
            required property string markup
            required property string cfi

            onClicked: {
                view.goTo(cfi);
                searchDialog.close();
            }

            contentItem: ColumnLayout {
                spacing: 0

                QQC2.Label {
                    Layout.fillWidth: true
                    text: searchDelegate.sectionMarkup
                    wrapMode: Text.WordWrap
                    font: Kirigami.Theme.smallFont
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    text: searchDelegate.markup
                    wrapMode: Text.WordWrap
                }
            }
        }

        Shortcut {
            sequence: "Ctrl+F"
            onActivated: searchDialog.open()
        }
    }

    actions: [
        Kirigami.Action {
            text: i18nc("@action:intoolbar", "Search")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "system-search-symbolic"
            onTriggered: searchDialog.open()
        },
        Kirigami.Action {
            text: i18n("Book Details")
            displayHint: Kirigami.DisplayHint.IconOnly
            icon.name: "documentinfo"
            enabled: backend.metadata
            onTriggered: {
                applicationWindow().pageStack.pushDialogLayer(Qt.resolvedUrl("./BookDetailsPage.qml"), {
                    metadata: root.entry
                });
            }
        }
    ]

    SearchModel {
        id: searchResultModel

        onSearchTriggered: text => {
            if (view.bookReady) {
                view.runJavaScript(`reader.search(${JSON.stringify(text)})`);
            }
        }
    }

    Kirigami.PlaceholderMessage {
        anchors.centerIn: parent
        width: parent.width - Kirigami.Units.gridUnit * 4
        text: i18n("No book selected")
        visible: root.url === ''
        helpfulAction: Kirigami.Action {
            text: i18n("Open file")
            onTriggered: {
                const fileDialog = openFileDialog.createObject(QQC2.ApplicationWindow.overlay);
                fileDialog.accepted.connect(() => {
                    const file = fileDialog.file;
                    if (!file) {
                        return;
                    }
                    root.url = file;
                });
                fileDialog.open();
            }
        }
    }

    Component {
        id: openFileDialog

        FileDialog {
            id: root
            parentWindow: applicationWindow()
            title: i18n("Please choose a file")
            nameFilters: [i18nc("Name filter for EPUB files", "eBook files (*.epub *.cb* *.fb2 *.fb2zip)")]
        }
    }

    Connections {
        target: applicationWindow().contextDrawer
        function onGoTo(cfi) {
            view.goTo(cfi);
        }
    }

    Connections {
        target: Translator
        function onTranslationReady(translatedString) {
            backend.showTranslation(translatedString);
        }
    }

    Connections {
        target: EditorProcess
        function onEditedFileChanged(file) {
            if (file !== root.filename && root.url !== "file://" + file) {
                return;
            }

            reloadChangedBookTimer.restart();
        }
    }

    Timer {
        id: reloadChangedBookTimer
        interval: 500
        repeat: false
        onTriggered: root.reloadCurrentBook()
    }

    Window {
        id: translatorDialog
        visible: false
        title: i18nc("@title:window", "Translation")
        transientParent: applicationWindow()
        modality: Qt.NonModal
        flags: Qt.Dialog
        width: Kirigami.Units.gridUnit * 28
        height: Kirigami.Units.gridUnit * 14

        function open() {
            show();
            raise();
        }

        QQC2.Pane {
            anchors.fill: parent

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                QQC2.TextArea {
                    id: sourceText
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    readOnly: true
                    wrapMode: Text.Wrap
                }
            }
        }
    }

    FocusScope {
        id: focusScope
        anchors.fill: parent
        focus: true // Set focus to the FocusScope
        focusPolicy: Qt.StrongFocus
        onFocusChanged: {
            if (!focusScope.focus) {
                focusScope.forceActiveFocus();
            }
        }
        Keys.onPressed: event => {
            if (event.modifiers & Qt.ControlModifier) {
                if (event.key === Qt.Key_Left) {
                    view.prevSection();
                } else if (event.key === Qt.Key_Right) {
                    view.nextSection();
                } else if (event.key === Qt.Key_E) {
                    if (root.filename) {
                        EditorProcess.start('/usr/local/bin/ebook-edit-check', [root.filename]);
                    }
                }
            } else if (event.modifiers & Qt.AltModifier) {
                if (event.key === Qt.Key_Left) {
                    view.goBack();
                } else if (event.key === Qt.Key_Right) {
                    view.goForward();
                }
            } else {
                if (event.key === Qt.Key_Left) {
                    view.prev();
                } else if (event.key === Qt.Key_Right) {
                    view.next();
                }
            }
            event.accepted = true;
        }
    }

    WebEngineView {
        id: view
        anchors.fill: parent
        url: Qt.resolvedUrl("main.html")
        visible: root.url !== ''
        webChannel: channel
        property bool readerReady: false
        property bool bookReady: false
        property bool readerCanGoBack: false
        property bool readerCanGoForward: false

        settings.javascriptEnabled: true
        settings.localContentCanAccessRemoteUrls: true
        settings.localContentCanAccessFileUrls: true

        Component.onCompleted: view.zoomFactor = root.zoomLevel > 0 ? root.zoomLevel : 1.0

        onCertificateError: error => {
            const u = error.url;

            if (u.scheme === "https" && (u.host === "127.0.0.1" || u.host === "localhost") && u.port === 45962 && error.overridable) {
                console.warn("Accepting local bookserver TLS certificate:", error.description);
                error.acceptCertificate();
                return;
            }

            error.rejectCertificate();
        }
        onVisibleChanged: if (!visible) {
            root.bookClosed();
        }

        onJavaScriptConsoleMessage: (level, message, lineNumber, sourceID) => {
            console.error('WEB:', level, message, lineNumber, sourceID);
        }
        onLoadingChanged: if (loading) {
            readerReady = false;
            bookReady = false;
            readerCanGoBack = false;
            readerCanGoForward = false;
        }

        function next() {
            if (bookReady) {
                view.runJavaScript('reader.view.next()');
            }
        }

        function prev() {
            if (bookReady) {
                view.runJavaScript('reader.view.prev()');
            }
        }

        function nextSection() {
            if (bookReady) {
                view.runJavaScript('reader.view.nextSection()');
            }
        }

        function prevSection() {
            if (bookReady) {
                view.runJavaScript('reader.view.prevSection()');
            }
        }

        function goTo(cfi) {
            if (bookReady) {
                view.runJavaScript('reader.view.goTo("' + cfi + '")');
            }
        }

        function goBack() {
            if (bookReady && readerCanGoBack) {
                view.runJavaScript('reader.view.history.back()');
            }
        }

        function goForward() {
            if (bookReady && readerCanGoForward) {
                view.runJavaScript('reader.view.history.forward()');
            }
        }

        QQC2.Menu {
            id: selectionPopup
            Connections {
                target: backend
                function onSelectionChanged() {
                    Qt.callLater(selectionPopup.popup);
                }
            }

            QQC2.MenuItem {
                text: i18n("Copy")
                icon.name: 'edit-copy'
                onClicked: view.runJavaScript("selectionAction('copy')")
            }

            QQC2.MenuItem {
                text: i18n("Find")
                icon.name: 'search'
                onClicked: view.runJavaScript("selectionAction('find')")
            }

            QQC2.MenuItem {
                text: i18n("Edit at Text")
                icon.name: 'document-edit'
                enabled: root.filename !== "" && backend.selection && backend.selection.text
                onClicked: {
                    if (!backend.selection || !backend.selection.text) {
                        return;
                    }

                    EditorProcess.start('/usr/local/bin/ebook-edit-check', [root.filename, '--select-text', backend.selection.text]);
                    view.runJavaScript("selectionAction('edit-at-text')");
                }
            }
            QQC2.MenuItem {
                text: i18n("Translate")
                icon.name: 'edit-find-replace'
                onClicked: view.runJavaScript("selectionAction('translate')")
            }
        }
    }

    // Allows the user to move pages by rotating the wheel or zoom with Ctrl+wheel
    MouseArea {
        anchors.fill: view
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        onClicked: mouse => {
            if (mouse.button === Qt.BackButton) {
                view.goBack();
            } else if (mouse.button === Qt.ForwardButton) {
                view.goForward();
            }
        }
        onWheel: event => {
            if (event.modifiers & Qt.ControlModifier) {
                const dx = event.angleDelta.x;
                const dy = event.angleDelta.y;

                // Adjust page width with horizontal Ctrl-wheel
                if (Math.abs(dx) > Math.abs(dy)) {
                    const step = 25;
                    const newWidth = Config.maxWidth + (dx < 0 ? step : -step);
                    Config.maxWidth = Math.max(100, newWidth);
                    Config.save();
                    backend.applyStyle();
                } else {
                    // Zoom functionality with vertical Ctrl-wheel
                    if (dy > 0) {
                        view.zoomFactor = Math.min(view.zoomFactor + 0.1, 3.0);
                    } else if (dy < 0) {
                        view.zoomFactor = Math.max(view.zoomFactor - 0.1, 0.5);
                    }
                    root.zoomLevelSaved(view.zoomFactor);
                }
            } else {
                // Navigation functionality
                let dx = event.angleDelta.x;
                let dy = event.angleDelta.y;

                // ignore tiny movements
                if (Math.abs(dx) < 1 && Math.abs(dy) < 1)
                    return;

                if (Math.abs(dx) > Math.abs(dy)) {
                    if (dx > 0)
                        view.prevSection();
                    else if (dx < 0)
                        view.nextSection();
                } else {
                    if (dy > 0)
                        view.prev();
                    else if (dy < 0)
                        view.next();
                }
            }
        }
    }

    footer: QQC2.ToolBar {
        visible: backend.locationsReady
        contentItem: RowLayout {
            QQC2.ToolButton {
                id: progressButton
                text: i18nc("Book reading progress", "%1%", Math.round(backend.progress * 100))
                property bool isMenuOpen: false
                onClicked: {
                    isMenuOpen = !isMenuOpen;
                    if (isMenuOpen) {
                        menu.popup(progressButton, 0, -menu.height);
                    } else {
                        menu.close();
                    }
                }
                Accessible.role: Accessible.ButtonMenu

                property QQC2.Menu menu: QQC2.Menu {
                    width: Kirigami.Units.gridUnit * 10
                    height: Kirigami.Units.gridUnit * 15
                    closePolicy: QQC2.Popup.CloseOnEscape | QQC2.Popup.CloseOnPressOutsideParent
                    contentItem: ColumnLayout {
                        Kirigami.FormLayout {
                            Layout.fillWidth: true
                            QQC2.Label {
                                Kirigami.FormData.label: i18n("Time left in chapter:")
                                text: backend.timeInChapter ? Format.formatDuration(backend.timeInChapter * 1000) : i18n("Loading")
                            }
                            QQC2.Label {
                                Kirigami.FormData.label: i18n("Time left in book:")
                                text: backend.timeInBook ? Format.formatDuration(backend.timeInBook * 1000) : i18n("Loading")
                            }
                        }
                    }
                }
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.ToolButton {
                text: i18n("Back")
                display: QQC2.AbstractButton.IconOnly
                icon.name: "go-previous-view"
                enabled: view.readerCanGoBack
                onClicked: view.goBack()
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.ToolButton {
                text: i18n("Previous Page")
                display: QQC2.AbstractButton.IconOnly
                icon.name: "arrow-left"
                onClicked: view.prev()
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.Slider {
                padding: Kirigami.Units.smallSpacing
                value: backend.progress
                onValueChanged: {
                    if (pressed) {
                        backend.progress = value;
                    }
                }
                onPressedChanged: {
                    if (!pressed) {
                        backend.progress = value;
                        if (view.bookReady) {
                            view.runJavaScript(`reader.view.goToFraction(${value})`);
                        }
                    }
                }
                live: false
                Layout.fillWidth: true
            }
            QQC2.ToolButton {
                text: i18n("Forward")
                display: QQC2.AbstractButton.IconOnly
                icon.name: "go-next-view"
                enabled: view.readerCanGoForward
                onClicked: view.goForward()
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
            QQC2.ToolButton {
                text: i18n("Next Page")
                icon.name: "arrow-right"
                onClicked: view.next()
                display: QQC2.AbstractButton.IconOnly
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }
    }

    QtObject {
        id: backend
        WebChannel.id: "backend"
        property var selection: null
        property double progress: 0
        property var location
        property var locations: root.locations
        property bool locationsReady: false
        property var metadata: null
        property var top: ({})
        property string file: root.url
        property int timeInChapter: 0
        property int timeInBook: 0
        function get(script, callback) {
            return view.runJavaScript(`JSON.stringify(${script})`, callback);
        }
        function showTranslation(text) {
            sourceText.text = text;
            translatorDialog.open();
        }
        function dispatch(action) {
            switch (action.type) {
            case 'ready':
                const uiText = {
                    loc: i18n("Loc. %s of %s"),
                    page: i18n("Page %s of %s"),
                    pageWithoutTotal: i18n("Page %s"),
                    close: i18n("Close"),
                    references: {
                        "footnote": i18n("Footnote"),
                        "footnote-go": i18n("Go to Footnote"),
                        "endnote": i18n("Endnote"),
                        "endnote-go": i18n("Go to Endnote"),
                        "note": i18n("Note"),
                        "note-go": i18n("Go to Note"),
                        "glossary": i18n("Definition"),
                        "glossary-go": i18n("Go to Definition"),
                        "biblioentry": i18n("Bibliography"),
                        "biblioentry-go": i18n("Go to Bibliography")
                    }
                };

                view.readerReady = true;
                view.runJavaScript(`init({'uiText': ${JSON.stringify(uiText)}})`);
                root.reloadBook();
                break;
            case 'book-ready':
                view.bookReady = true;
                searchResultModel.clear();
                searchResultModel.loading = false;

                const {
                    book
                } = action.payload;
                if (book && book.toc) {
                    applicationWindow().contextDrawer.model.importFromJson(JSON.stringify(book.toc));
                } else {
                    console.warn('Book or TOC not available');
                }

                applyStyle();

                const metadata = action.payload.book.metadata;
                if (metadata) {
                    backend.metadata = metadata;
                    root.bookReady(backend.metadata.title);
                } else {
                    view.next();
                }
                break;
            case 'book-error':
                console.error('Book error', action.payload);
                break;
            case 'selection':
                if (action.payload.action === 'copy') {
                    Clipboard.saveText(action.payload.text);
                } else if (action.payload.action === 'translate') {
                    backend.showTranslation(i18n("Translating..."));
                    Translator.translate(action.payload.text);
                } else {
                    backend.selection = action.payload;
                }
                break;
            case 'selection-find':
                searchDialog.text = action.payload.text;
                searchResultModel.search(action.payload.text);
                searchResultModel.loading = true;
                searchDialog.open();
                break;
            case 'show-selection':
                backend.selection = action.payload;
                break;
            case 'relocate':
                backend.progress = action.payload.fraction;
                backend.location = action.payload;
                root.currentLocation = action.payload.cfi;
                backend.timeInChapter = Math.round(action.payload.time.section * 60);
                backend.timeInBook = Math.round(action.payload.time.total * 60);
                root.relocated(action.payload.cfi, action.payload.fraction * 100);
                backend.locationsReady = true;
                break;
            case 'history-index-change':
                view.readerCanGoBack = action.payload.canGoBack;
                view.readerCanGoForward = action.payload.canGoForward;
                break;
            case 'find-results':
                searchResultModel.resultFound(action.payload.query, action.payload.results);
                break;
            case 'external-link':
                Qt.openUrlExternally(action.payload.href);
                break;
            }
        }

        function applyStyle() {
            if (!view.bookReady) {
                return;
            }

            const getIbooksInternalTheme = bgColor => {
                const red = bgColor.r;
                const green = bgColor.g;
                const blue = bgColor.b;
                const l = 0.299 * red + 0.587 * green + 0.114 * blue;
                if (l < 0.3)
                    return 'Night';
                else if (l < 0.7)
                    return 'Gray';
                else if (red > green && green > blue)
                    return 'Sepia';
                else
                    return 'White';
            };
            const fontDesc = Config.defaultFont;
            const fontFamily = fontDesc.family;
            const fontSizePt = fontDesc.pointSize;
            const fontSize = fontDesc.pixelSize;
            let fontWeight = 400;
            const fontStyle = fontDesc.styleName;
            const maxWidth = Config.maxWidth || 720;
            const defaultTheme = Config.invert ? {
                light: {
                    fg: "#ffffff",
                    bg: "#000000",
                    link: "#8ab4f8"
                },
                dark: {
                    fg: "#ffffff",
                    bg: "#000000",
                    link: "#8ab4f8"
                },
                inverted: {
                    fg: "#ffffff",
                    bg: "#000000",
                    link: "#8ab4f8"
                }
            } : {
                light: {
                    fg: "#000000",
                    bg: "#ffffff",
                    link: "#0000ee"
                },
                dark: {
                    fg: "#000000",
                    bg: "#ffffff",
                    link: "#0000ee"
                },
                inverted: {
                    fg: "#ffffff",
                    bg: "#000000",
                    link: "#8ab4f8"
                }
            };
            const kdeForegroundColor = Config.invert ? Kirigami.Theme.backgroundColor.toString() : Kirigami.Theme.textColor.toString();
            const kdeBackgroundColor = Config.invert ? Kirigami.Theme.textColor.toString() : Kirigami.Theme.backgroundColor.toString();
            const kdeLinkColor = Kirigami.Theme.highlightColor.toString();
            const kdeTheme = {
                light: {
                    fg: kdeForegroundColor,
                    bg: kdeBackgroundColor,
                    link: kdeLinkColor
                },
                dark: {
                    fg: kdeForegroundColor,
                    bg: kdeBackgroundColor,
                    link: kdeLinkColor
                },
                inverted: {
                    fg: kdeForegroundColor,
                    bg: kdeBackgroundColor,
                    link: kdeLinkColor
                }
            };
            const invertStylesheet = `
                html, body {
                    color: #ffffff !important;
                    background: #000000 !important;
                }
                *, *::before, *::after {
                    color: inherit !important;
                    border-color: currentColor !important;
                    background-color: #000000 !important;
                }
                a:any-link {
                    color: #8ab4f8 !important;
                }
                svg, img {
                    background-color: transparent !important;
                    filter: invert(1) hue-rotate(180deg) !important;
                }
                svg.not_inverse,
                img.not_inverse,
                .not_inverse svg,
                .not_inverse img {
                    filter: none !important;
                }
                mjx-container,
                mjx-container *,
                mjx-container svg,
                mjx-container svg * {
                    color: #ffffff !important;
                    background: transparent !important;
                    background-color: transparent !important;
                    border-color: currentColor !important;
                    stroke: currentColor !important;
                }
                mjx-container svg {
                    filter: none !important;
                    fill: currentColor !important;
                }
                mjx-container svg [fill="none"] {
                    fill: none !important;
                }
                mjx-container svg [stroke] {
                    stroke: currentColor !important;
                }
            `;

            const transparentStylesheet = `
                html, body {
                    background: transparent !important;
                    background-color: transparent !important;
                }
                body * {
                    background: transparent !important;
                    background-color: transparent !important;
                    background-image: none !important;
                }
            `;

            const hasReaderBackground = Config.readerBackgroundPath.length > 0;
            const readerBackgroundImage = hasReaderBackground && Config.invert ? 'linear-gradient(rgba(0, 0, 0, 0.86), rgba(0, 0, 0, 0.86)), url("http://127.0.0.1:45961/static/background-image")' : hasReaderBackground ? 'linear-gradient(rgba(255, 255, 255, 0.86), rgba(255, 255, 255, 0.86)), url("http://127.0.0.1:45961/static/background-image")' : '';
            const readerBackgroundFilter = 'none';
            const readerBackgroundStylesheet = hasReaderBackground ? `
                body {
                    isolation: isolate;
                    position: relative;
                }
                body::before {
                    content: "";
                    position: fixed;
                    inset: 0;
                    z-index: 0;
                    pointer-events: none;
                    background-image: ${readerBackgroundImage};
                    background-position: center center;
                    background-repeat: no-repeat;
                    background-size: cover;
                    filter: ${readerBackgroundFilter};
                }
                body > * {
                    position: relative;
                    z-index: 1;
                }
            ` : '';
            const invertBackgroundStylesheet = hasReaderBackground ? `
                html, body {
                    color: #ffffff !important;
                    background: transparent !important;
                    background-color: transparent !important;
                }
                *, *::before, *::after {
                    color: inherit !important;
                    border-color: currentColor !important;
                    background-color: transparent !important;
                }
                body * {
                    background: transparent !important;
                    background-color: transparent !important;
                    background-image: none !important;
                }
                a:any-link {
                    color: #8ab4f8 !important;
                }
                svg, img {
                    background-color: transparent !important;
                    filter: invert(1) hue-rotate(180deg) !important;
                }
                svg.not_inverse,
                img.not_inverse,
                .not_inverse svg,
                .not_inverse img {
                    filter: none !important;
                }
                mjx-container,
                mjx-container *,
                mjx-container svg,
                mjx-container svg * {
                    color: #ffffff !important;
                    background: transparent !important;
                    background-color: transparent !important;
                    border-color: currentColor !important;
                    stroke: currentColor !important;
                }
                mjx-container svg {
                    filter: none !important;
                    fill: currentColor !important;
                }
                mjx-container svg [fill="none"] {
                    fill: none !important;
                }
                mjx-container svg [stroke] {
                    stroke: currentColor !important;
                }
            ` : invertStylesheet;

            const style = {
                layout: {
                    gap: 0.06,
                    maxInlineSize: maxWidth,
                    maxBlockSize: maxWidth * 2,
                    maxColumnCount: 2,
                    flow: 'paginated' // 'scrolled'
                    ,
                    animated: true
                },
                style: {
                    lineHeight: 1.5,
                    justify: Config.justify,
                    hyphenate: Config.hyphenate,
                    invert: Config.invert,
                    theme: Config.kdeTheming ? kdeTheme : defaultTheme,
                    overrideFont: !Config.usePublisherFont,
                    userStylesheet: (Config.invert ? invertBackgroundStylesheet : transparentStylesheet) + readerBackgroundStylesheet,
                    readerBackgroundImage: readerBackgroundImage,
                    readerBackgroundFilter: readerBackgroundFilter
                }
            };

            view.runJavaScript(`reader.setAppearance(${JSON.stringify(style)})`);
        }
    }

    WebChannel {
        id: channel
        registeredObjects: [backend]
    }

    Shortcut {
        sequence: "Right"
        onActivated: view.next()
    }

    Shortcut {
        sequence: "Left"
        onActivated: view.prev()
    }

    Shortcut {
        sequence: "SHIFT+Right"
        onActivated: view.next()
    }

    Shortcut {
        sequence: "SHIFT+Left"
        onActivated: view.prev()
    }

    Shortcut {
        sequence: "Alt+Left"
        onActivated: view.goBack()
    }

    Shortcut {
        sequence: "Alt+Right"
        onActivated: view.goForward()
    }

    Shortcut {
        sequence: "Ctrl+R"
        enabled: !view.loading
        onActivated: root.reloadCurrentBook()
    }

    Shortcut {
        sequence: "Ctrl+B"
        onActivated: EditorProcess.startDetached(applicationFilePath, [])
    }
}
