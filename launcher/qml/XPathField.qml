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

    signal pathEdited(string path)

    onPathValueChanged: editingText = pathValue

    Layout.fillWidth: true
    spacing: Theme.spaceXs

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
            onClicked: folderDialog.open()
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
        onAccepted: {
            var nativePath = launcherBridge.toLocalPath(selectedFolder)
            root.editingText = nativePath
            root.pathEdited(nativePath)
        }
    }
}
