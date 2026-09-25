import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.folderlistmodel

// Browses real files from the configured screenshots directory. Folder I/O is
// owned by FolderListModel; the page copies only lightweight metadata into a
// filtered ListModel and lets GridView virtualize thumbnail delegates.
Item {
    id: root

    property string searchText: ""
    property bool refreshing: false
    property string pendingDeletePath: ""
    property string pendingDeleteName: ""
    property int viewerIndex: -1
    property real viewerZoom: 1.0

    readonly property string capturesPath: String(launcherBridge.defaultScreenshotsPath())
    readonly property string capturesUrl: "file:///" + root.capturesPath.replace(/\\/g, "/")
    readonly property bool loading: root.refreshing || captureFiles.status === FolderListModel.Loading
    readonly property int gridColumns: Math.max(1, Math.floor(Math.max(1, captureGrid.width) / 300))

    ListModel { id: visibleCaptures }

    FolderListModel {
        id: captureFiles
        folder: root.capturesUrl
        showDirs: false
        showDotAndDotDot: false
        nameFilters: ["*.png", "*.jpg", "*.jpeg", "*.bmp", "*.webp"]
        sortField: FolderListModel.Time
        sortReversed: true

        onCountChanged: captureSyncTimer.restart()
        onStatusChanged: {
            if (status === FolderListModel.Ready) {
                root.refreshing = false
                captureSyncTimer.restart()
            }
        }
    }

    function fileUrl(path) {
        return "file:///" + String(path || "").replace(/\\/g, "/")
    }

    function formatBytes(value) {
        var bytes = Number(value || 0)
        if (bytes < 1024) return Math.round(bytes) + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KiB"
        return (bytes / (1024 * 1024)).toFixed(1) + " MiB"
    }

    function matchesSearch(name) {
        var needle = root.searchText.trim().toLowerCase()
        return needle.length === 0 || String(name).toLowerCase().indexOf(needle) !== -1
    }

    function sourceCapture(index) {
        return ({
            fileName: String(captureFiles.get(index, "fileName") || ""),
            filePath: String(captureFiles.get(index, "filePath") || ""),
            fileSize: Number(captureFiles.get(index, "fileSize") || 0),
            fileModified: captureFiles.get(index, "fileModified")
        })
    }

    function visibleIndexForPath(path, firstIndex) {
        var target = String(path || "")
        for (var i = Math.max(0, Number(firstIndex || 0)); i < visibleCaptures.count; ++i) {
            if (String(visibleCaptures.get(i).filePath || "") === target) return i
        }
        return -1
    }

    function syncVisibleCaptures() {
        if (captureFiles.status !== FolderListModel.Ready) return

        var previousOffset = captureGrid.contentY
        var desired = []
        var wanted = ({})
        for (var i = 0; i < captureFiles.count; ++i) {
            var item = root.sourceCapture(i)
            if (!root.matchesSearch(item.fileName)) continue
            desired.push(item)
            wanted[item.filePath] = true
        }

        for (var oldIndex = visibleCaptures.count - 1; oldIndex >= 0; --oldIndex) {
            if (!wanted[String(visibleCaptures.get(oldIndex).filePath || "")])
                visibleCaptures.remove(oldIndex)
        }

        for (var targetIndex = 0; targetIndex < desired.length; ++targetIndex) {
            var desiredItem = desired[targetIndex]
            var existingIndex = root.visibleIndexForPath(desiredItem.filePath, targetIndex)
            if (existingIndex < 0) {
                visibleCaptures.insert(targetIndex, desiredItem)
            } else {
                if (existingIndex !== targetIndex)
                    visibleCaptures.move(existingIndex, targetIndex, 1)
                visibleCaptures.set(targetIndex, desiredItem)
            }
        }
        while (visibleCaptures.count > desired.length)
            visibleCaptures.remove(visibleCaptures.count - 1)

        if (captureViewer.visible) {
            if (visibleCaptures.count === 0) {
                captureViewer.close()
                root.viewerIndex = -1
            } else {
                root.viewerIndex = Math.max(0, Math.min(visibleCaptures.count - 1, root.viewerIndex))
            }
        }

        Qt.callLater(function() {
            var maximum = Math.max(0, captureGrid.contentHeight - captureGrid.height)
            captureGrid.contentY = Math.max(0, Math.min(maximum, previousOffset))
        })
    }

    function refresh() {
        if (root.refreshing) return
        root.refreshing = true
        // FolderListModel watches ordinary changes itself. Temporarily
        // clearing the folder provides an explicit rescan for Ctrl+R / Refresh.
        captureFiles.folder = ""
        captureRefreshTimer.restart()
    }

    function requestDelete(path, name) {
        root.pendingDeletePath = String(path || "")
        root.pendingDeleteName = String(name || "Capture")
        deleteConfirm.open()
    }

    function currentCapture() {
        return root.viewerIndex >= 0 && root.viewerIndex < visibleCaptures.count
            ? visibleCaptures.get(root.viewerIndex) : ({})
    }

    function openViewer(index) {
        if (index < 0 || index >= visibleCaptures.count) return
        root.viewerIndex = index
        root.viewerZoom = 1.0
        captureViewer.open()
    }

    function stepViewer(delta) {
        if (visibleCaptures.count === 0) return
        root.viewerIndex = Math.max(0, Math.min(visibleCaptures.count - 1, root.viewerIndex + delta))
        root.viewerZoom = 1.0
        captureGrid.currentIndex = root.viewerIndex
        captureGrid.positionViewAtIndex(root.viewerIndex, GridView.Contain)
    }

    function adjustViewerZoom(delta) {
        root.viewerZoom = Math.max(1.0, Math.min(4.0, root.viewerZoom + delta))
    }

    function focusedCapture() {
        var index = Math.max(0, Math.min(visibleCaptures.count - 1, captureGrid.currentIndex))
        return visibleCaptures.count > 0 ? visibleCaptures.get(index) : ({})
    }

    function openContextMenuForFocusedItem() {
        if (visibleCaptures.count === 0) return
        captureMenu.openAt(captureGrid, Math.max(0, captureGrid.width - captureMenu.menuWidth - Theme.spaceSm), Theme.spaceSm)
    }

    function triggerSecondaryAction() {
        var capture = root.focusedCapture()
        if (String(capture.filePath || "").length > 0)
            root.requestDelete(capture.filePath, capture.fileName)
    }

    function runCaptureAction(actionId) {
        var capture = root.focusedCapture()
        if (String(capture.filePath || "").length === 0) return
        if (actionId === "view") root.openViewer(captureGrid.currentIndex)
        else if (actionId === "external") Qt.openUrlExternally(root.fileUrl(capture.filePath))
        else if (actionId === "copy") {
            launcherBridge.copyText(capture.filePath)
            launcherBridge.notify("Copied", "The capture path was copied to the clipboard.")
        } else if (actionId === "folder") launcherBridge.openFolder(root.capturesPath)
        else if (actionId === "delete") root.requestDelete(capture.filePath, capture.fileName)
    }

    function handleDirectionalNavigation(direction) {
        if (!captureGrid.activeFocus || visibleCaptures.count === 0) return false
        var delta = 0
        if (direction === "left") delta = -1
        else if (direction === "right") delta = 1
        else if (direction === "up") delta = -root.gridColumns
        else if (direction === "down") delta = root.gridColumns
        else return false
        captureGrid.currentIndex = Math.max(0, Math.min(visibleCaptures.count - 1,
                                                        captureGrid.currentIndex + delta))
        captureGrid.positionViewAtIndex(captureGrid.currentIndex, GridView.Contain)
        return true
    }

    function movePage(forward) {
        if (visibleCaptures.count === 0) return
        var rows = Math.max(1, Math.floor(captureGrid.height / Math.max(1, captureGrid.cellHeight)))
        var next = captureGrid.currentIndex + (forward ? 1 : -1) * rows * root.gridColumns
        captureGrid.currentIndex = Math.max(0, Math.min(visibleCaptures.count - 1, next))
        captureGrid.positionViewAtIndex(captureGrid.currentIndex, GridView.Contain)
    }

    onSearchTextChanged: captureSearchTimer.restart()

    Timer {
        id: captureSyncTimer
        interval: 70
        repeat: false
        onTriggered: root.syncVisibleCaptures()
    }

    Timer {
        id: captureSearchTimer
        interval: 100
        repeat: false
        onTriggered: root.syncVisibleCaptures()
    }

    Timer {
        id: captureRefreshTimer
        interval: 1
        repeat: false
        onTriggered: captureFiles.folder = root.capturesUrl
    }

    Shortcut {
        sequence: "Ctrl+R"
        enabled: root.visible
        onActivated: root.refresh()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: Theme.spaceMd

        XSectionHeader {
            title: "Captures"
            prominent: true
            description: "Screenshots saved to " + root.capturesPath + ". Video capture is not implemented yet."
        }

        XPanel {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                spacing: Theme.spaceMd

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    Text {
                        Layout.fillWidth: true
                        text: root.loading ? "Loading captures…"
                            : visibleCaptures.count + (visibleCaptures.count === 1 ? " capture" : " captures")
                        color: Theme.textMuted
                        font.pixelSize: Theme.typeCaption
                    }
                    XButton {
                        text: root.refreshing ? "Refreshing…" : "Refresh"
                        enabled: !root.refreshing
                        onClicked: root.refresh()
                    }
                    XButton { text: "Open Folder"; onClicked: launcherBridge.openFolder(root.capturesPath) }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                EmptyState {
                    visible: root.loading
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    glyph: "◇"
                    title: "Loading captures…"
                    description: "Reading screenshot metadata from the configured captures folder."
                    primaryText: ""
                    secondaryText: ""
                }

                EmptyState {
                    visible: !root.loading && visibleCaptures.count === 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    glyph: "◇"
                    title: root.searchText.trim().length > 0 ? "No matching captures" : "No captures yet"
                    description: root.searchText.trim().length > 0
                        ? "Try a different capture search."
                        : "Screenshots will appear here once a game session saves one to the configured captures folder. Video recording is not implemented yet."
                    primaryText: root.searchText.trim().length > 0 ? "" : "Open Captures Folder"
                    secondaryText: ""
                    onPrimaryClicked: launcherBridge.openFolder(root.capturesPath)
                }

                GridView {
                    id: captureGrid
                    visible: !root.loading && visibleCaptures.count > 0
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: visibleCaptures
                    cellWidth: width / Math.max(1, root.gridColumns)
                    cellHeight: Math.max(210, Math.min(270, cellWidth * 0.78))
                    cacheBuffer: Math.max(cellHeight * 2, 480)
                    reuseItems: true
                    boundsBehavior: Flickable.StopAtBounds
                    focus: visible
                    activeFocusOnTab: true
                    keyNavigationEnabled: true
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    Keys.onReturnPressed: root.openViewer(currentIndex)
                    Keys.onEnterPressed: root.openViewer(currentIndex)
                    Keys.onDeletePressed: root.triggerSecondaryAction()

                    delegate: Item {
                        id: captureCell
                        required property int index
                        required property string fileName
                        required property string filePath
                        required property real fileSize
                        required property var fileModified
                        width: captureGrid.cellWidth
                        height: captureGrid.cellHeight

                        XPanel {
                            anchors.fill: parent
                            anchors.margins: Theme.spaceXs
                            decorated: false
                            color: Theme.highContrast ? Theme.surfaceAlt
                                : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)
                            border.color: captureGrid.currentIndex === captureCell.index ? Theme.accent : Theme.border

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: Theme.spaceSm
                                spacing: Theme.spaceSm

                                CoverImage {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    Layout.minimumHeight: 110
                                    source: root.fileUrl(captureCell.filePath)
                                    fitMode: "cover"
                                    asynchronous: true
                                    decodeHeadroom: 1.35
                                    maximumDecodeDimension: 1280
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: captureCell.fileName
                                    color: Theme.text
                                    font.pixelSize: Theme.typeBody
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideMiddle
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.formatBytes(captureCell.fileSize) + " • "
                                        + Qt.formatDateTime(captureCell.fileModified, "yyyy-MM-dd hh:mm")
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeCaption
                                    elide: Text.ElideRight
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.spaceXs
                                    XButton {
                                        Layout.fillWidth: true
                                        text: "View"
                                        variant: "primary"
                                        onClicked: root.openViewer(captureCell.index)
                                    }
                                    XButton {
                                        text: "Copy Path"
                                        onClicked: {
                                            launcherBridge.copyText(captureCell.filePath)
                                            launcherBridge.notify("Copied", "The capture path was copied to the clipboard.")
                                        }
                                    }
                                    XButton {
                                        text: "Delete"
                                        variant: "danger"
                                        onClicked: root.requestDelete(captureCell.filePath, captureCell.fileName)
                                    }
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.RightButton
                                cursorShape: Qt.PointingHandCursor
                                onClicked: function(mouse) {
                                    captureGrid.currentIndex = captureCell.index
                                    captureMenu.openAt(captureCell, mouse.x, mouse.y)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    XActionMenu {
        id: captureMenu
        parent: root
        z: 1001
        actions: [
            { id: "view", label: "View capture", icon: "▣" },
            { id: "external", label: "Open externally", icon: "↗" },
            { id: "copy", label: "Copy path", icon: "⧉" },
            { id: "folder", label: "Open captures folder", icon: "□" },
            { id: "delete", label: "Delete capture", icon: "×", separatorBefore: true, destructive: true }
        ]
        onActionTriggered: function(actionId) { root.runCaptureAction(actionId) }
    }

    Popup {
        id: captureViewer
        parent: Overlay.overlay
        width: Math.min(1180, parent ? parent.width - Theme.space2Xl * 2 : 1180)
        height: Math.min(820, parent ? parent.height - Theme.space2Xl * 2 : 820)
        x: parent ? Math.round((parent.width - width) / 2) : 0
        y: parent ? Math.round((parent.height - height) / 2) : 0
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionNormal } }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast } }
        onOpened: {
            viewerStage.forceActiveFocus()
            NavigationGuard.pushModal()
        }
        onClosed: NavigationGuard.popModal()

        Overlay.modal: Rectangle { color: Theme.overlay }
        background: Rectangle {
            color: Theme.surfaceRaised
            radius: Theme.dialogRadius
            border.width: Theme.borderWidth
            border.color: Theme.border
        }

        Connections {
            target: launcherBridge
            enabled: captureViewer.visible
            function onFrontendAction(action) {
                if (action === "left" || action === "pageBack") root.stepViewer(-1)
                else if (action === "right" || action === "pageForward") root.stepViewer(1)
                else if (action === "scrollUp") root.adjustViewerZoom(0.25)
                else if (action === "scrollDown") root.adjustViewerZoom(-0.25)
                else if (action === "secondary") root.viewerZoom = 1.0
                else if (action === "cancel" || action === "back") captureViewer.close()
            }
        }

        contentItem: ColumnLayout {
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.spaceMd
                spacing: Theme.spaceSm
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: String(root.currentCapture().fileName || "Capture")
                        color: Theme.text
                        font.pixelSize: Theme.typeSubtitle
                        font.weight: Font.DemiBold
                        elide: Text.ElideMiddle
                    }
                    Text {
                        text: (root.viewerIndex + 1) + " of " + visibleCaptures.count + "  •  " + Math.round(root.viewerZoom * 100) + "%"
                        color: Theme.textMuted
                        font.pixelSize: Theme.typeCaption
                    }
                }
                XIconButton { glyph: "−"; tooltip: "Zoom out"; enabled: root.viewerZoom > 1.0; onClicked: root.adjustViewerZoom(-0.25) }
                XButton { text: "Fit"; variant: "ghost"; enabled: root.viewerZoom !== 1.0; onClicked: root.viewerZoom = 1.0 }
                XIconButton { glyph: "+"; tooltip: "Zoom in"; enabled: root.viewerZoom < 4.0; onClicked: root.adjustViewerZoom(0.25) }
                XIconButton { iconName: "close"; glyph: "×"; tooltip: "Close viewer"; onClicked: captureViewer.close() }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

            Item {
                id: viewerStage
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: Theme.spaceMd
                clip: true
                focus: true
                activeFocusOnTab: true
                Accessible.role: Accessible.Pane
                Accessible.name: "Capture viewer"

                CoverImage {
                    anchors.fill: parent
                    source: root.fileUrl(root.currentCapture().filePath)
                    fitMode: "contain"
                    zoom: root.viewerZoom
                    asynchronous: true
                    decodeHeadroom: 1.5
                    maximumDecodeDimension: 4096
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.NoButton
                    onWheel: function(wheel) {
                        root.adjustViewerZoom(wheel.angleDelta.y > 0 ? 0.25 : -0.25)
                        wheel.accepted = true
                    }
                }

                Keys.onPressed: function(event) {
                    if (event.key === Qt.Key_Left) { root.stepViewer(-1); event.accepted = true }
                    else if (event.key === Qt.Key_Right) { root.stepViewer(1); event.accepted = true }
                    else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) { root.adjustViewerZoom(0.25); event.accepted = true }
                    else if (event.key === Qt.Key_Minus) { root.adjustViewerZoom(-0.25); event.accepted = true }
                    else if (event.key === Qt.Key_0 || event.key === Qt.Key_Home) { root.viewerZoom = 1.0; event.accepted = true }
                }

                XIconButton {
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "chevron-left"
                    glyph: "‹"
                    tooltip: "Previous capture"
                    variant: "filled"
                    enabled: root.viewerIndex > 0
                    onClicked: root.stepViewer(-1)
                }
                XIconButton {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "chevron-right"
                    glyph: "›"
                    tooltip: "Next capture"
                    variant: "filled"
                    enabled: root.viewerIndex + 1 < visibleCaptures.count
                    onClicked: root.stepViewer(1)
                }
            }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.spaceMd
                spacing: Theme.spaceSm
                XButton { text: "Open externally"; onClicked: Qt.openUrlExternally(root.fileUrl(root.currentCapture().filePath)) }
                XButton { text: "Copy path"; onClicked: root.runCaptureAction("copy") }
                Item { Layout.fillWidth: true }
                XButton {
                    text: "Delete"
                    variant: "danger"
                    onClicked: root.requestDelete(root.currentCapture().filePath, root.currentCapture().fileName)
                }
            }
        }
    }

    XConfirmDialog {
        id: deleteConfirm
        title: "Delete “" + root.pendingDeleteName + "”?"
        message: "This permanently deletes the screenshot from the captures folder. This cannot be undone."
        confirmText: "Delete Capture"
        destructive: true
        onConfirmed: {
            if (launcherBridge.deleteCaptureFile(root.pendingDeletePath)) {
                launcherBridge.notify("Capture deleted", root.pendingDeleteName)
                captureSyncTimer.restart()
            }
            root.pendingDeletePath = ""
            root.pendingDeleteName = ""
        }
    }
}
