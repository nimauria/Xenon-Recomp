import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qt.labs.folderlistmodel

// Xenon does not record video or take screenshots yet (no capture backend
// exists in the runtime host) - this page does not pretend otherwise. What
// it genuinely does is browse the real, already-configured captures folder
// (Settings > Paths > Screenshots) for image files, so it is immediately
// useful the moment capture support lands without further launcher changes,
// and honestly empty until then.
Item {
    id: root

    property string searchText: ""

    readonly property string capturesPath: String(launcherBridge.defaultScreenshotsPath())
    readonly property string capturesUrl: "file:///" + root.capturesPath.replace(/\\/g, "/")

    FolderListModel {
        id: captureFiles
        folder: root.capturesUrl
        showDirs: false
        showDotAndDotDot: false
        nameFilters: ["*.png", "*.jpg", "*.jpeg", "*.bmp", "*.webp"]
        sortField: FolderListModel.Time
        sortReversed: true
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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.pageMargin
        spacing: Theme.spaceMd

        XSectionHeader {
            title: "Captures"
            description: "Screenshots saved to " + root.capturesPath + ". Video capture is not implemented yet."
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Text {
                Layout.fillWidth: true
                text: captureFiles.count + (captureFiles.count === 1 ? " capture" : " captures")
                color: Theme.textMuted
                font.pixelSize: Theme.typeCaption
            }
            XButton { text: "Open Folder"; onClicked: launcherBridge.openFolder(root.capturesPath) }
        }

        EmptyState {
            visible: captureFiles.count === 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            glyph: "◇"
            title: "No captures yet"
            description: "Screenshots will appear here once a game session saves one to the configured captures folder. Video recording is not implemented yet."
            primaryText: "Open Captures Folder"
            secondaryText: ""
            onPrimaryClicked: launcherBridge.openFolder(root.capturesPath)
        }

        ScrollView {
            visible: captureFiles.count > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: parent.width
                spacing: Theme.spaceSm

                Repeater {
                    model: captureFiles
                    delegate: XPanel {
                        required property string fileName
                        required property string filePath
                        required property int fileSize
                        required property var fileModified
                        visible: root.matchesSearch(fileName)
                        Layout.fillWidth: true
                        Layout.preferredHeight: visible ? 72 : 0
                        color: Theme.highContrast ? Theme.surfaceAlt : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.spaceMd
                            spacing: Theme.spaceMd

                            CoverImage {
                                Layout.preferredWidth: 56
                                Layout.preferredHeight: 40
                                source: "file:///" + filePath.replace(/\\/g, "/")
                                fitMode: "cover"
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Text { Layout.fillWidth: true; text: fileName; color: Theme.text; font.pixelSize: Theme.typeBody; elide: Text.ElideRight }
                                Text { text: root.formatBytes(fileSize) + " • " + Qt.formatDateTime(fileModified, "yyyy-MM-dd hh:mm"); color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                            }

                            XButton { text: "Open"; onClicked: Qt.openUrlExternally("file:///" + filePath.replace(/\\/g, "/")) }
                            XButton { text: "Copy Path"; onClicked: { launcherBridge.copyText(filePath); launcherBridge.notify("Copied", "The file path was copied to the clipboard.") } }
                            XButton {
                                text: "Delete"
                                variant: "danger"
                                onClicked: launcherBridge.deleteCaptureFile(filePath)
                            }
                        }
                    }
                }
            }
        }
    }
}
