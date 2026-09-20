import QtQuick
import QtQuick.Dialogs
import QtQuick.Layouts

ColumnLayout {
    id: root

    property string label: "Path"
    property string pathValue: ""
    property string placeholderText: "Not configured"
    property bool readOnly: false
    property bool allowClear: false
    property string clearText: "Use Default"
    property string helperText: ""
    property string editingText: pathValue

    // Picker behaviour. Existing settings/profile uses remain directory pickers,
    // while callers such as FilesystemPage can explicitly request a file.
    property string mode: "directory" // "directory" or "file"
    property var filters: []

    // Compatibility/readback surface for callers that treat XPathField like a
    // regular text field (for example FilesystemPage when invoking the bridge).
    readonly property string text: editingText

    signal pathEdited(string path)

    onPathValueChanged: editingText = pathValue

    Layout.fillWidth: true
    spacing: Theme.spaceXs

    function commitSelectedUrl(url) {
        var nativePath = launcherBridge.toLocalPath(url)
        root.editingText = nativePath
        root.pathEdited(nativePath)
    }

    Text {
        text: root.label
        color: Theme.text
        font.pixelSize: Theme.typeCaption
        font.weight: Font.DemiBold
    }

    GridLayout {
        id: pathGrid
        Layout.fillWidth: true
        readonly property bool stacked: width > 0 && (width < 620 || Theme.textScale >= 1.6)
        columns: stacked ? 1 : (root.allowClear ? 3 : 2)
        columnSpacing: Theme.spaceSm
        rowSpacing: Theme.spaceSm

        XTextField {
            id: pathField
            Layout.fillWidth: true
            text: root.editingText
            readOnly: root.readOnly
            placeholderText: root.placeholderText
            Accessible.name: root.label
            onEditingFinished: {
                root.editingText = text
                root.pathEdited(text)
            }
        }

        XButton {
            Layout.fillWidth: pathGrid.stacked
            text: "Browse…"
            implicitWidth: 108
            enabled: !root.readOnly
            onClicked: {
                if (root.mode === "file")
                    fileDialog.open()
                else
                    folderDialog.open()
            }
        }

        XButton {
            visible: root.allowClear
            Layout.fillWidth: pathGrid.stacked
            text: root.clearText
            implicitWidth: 112
            enabled: !root.readOnly
            onClicked: {
                root.editingText = ""
                root.pathEdited("")
            }
        }
    }

    Text {
        visible: root.helperText.length > 0
        Layout.fillWidth: true
        text: root.helperText
        color: Theme.textMuted
        wrapMode: Text.WordWrap
        font.pixelSize: Theme.typeCaption
        lineHeight: 1.25
    }

    FolderDialog {
        id: folderDialog
        title: "Select " + root.label
        onAccepted: root.commitSelectedUrl(selectedFolder)
    }

    FileDialog {
        id: fileDialog
        title: "Select " + root.label
        fileMode: FileDialog.OpenFile
        nameFilters: root.filters.length > 0 ? root.filters : ["All files (*)"]
        onAccepted: root.commitSelectedUrl(selectedFile)
    }
}
