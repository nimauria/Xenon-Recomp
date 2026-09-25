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
                                        text: "Open"
                                        variant: "primary"
                                        onClicked: Qt.openUrlExternally(root.fileUrl(captureCell.filePath))
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
                        }
                    }
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
