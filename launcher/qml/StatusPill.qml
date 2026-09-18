import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property string label: ""
    property color tone: Theme.textMuted

    implicitHeight: 28
    implicitWidth: row.implicitWidth + Theme.spaceLg
    radius: implicitHeight / 2
    color: Theme.highContrast
        ? Theme.surfaceAlt
        : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)
    border.width: Theme.borderWidth
    border.color: Theme.border

    Accessible.role: Accessible.StaticText
    Accessible.name: root.label

    function statusIcon() {
        var toneString = String(root.tone)
        if (toneString === String(Theme.success))
            return "qrc:/theme-art/decor/neutral/status_ready.svg"
        if (toneString === String(Theme.warning))
            return "qrc:/theme-art/decor/neutral/status_processing.svg"
        if (toneString === String(Theme.danger))
            return "qrc:/theme-art/decor/neutral/status_error.svg"
        if (toneString === String(Theme.textMuted))
            return "qrc:/theme-art/decor/neutral/status_offline.svg"
        return ""
    }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceSm

        Image {
            id: statusImage
            Layout.preferredWidth: 12
            Layout.preferredHeight: 12
            source: root.statusIcon()
            visible: source.toString().length > 0
            fillMode: Image.PreserveAspectFit
            smooth: true
        }

        Rectangle {
            visible: !statusImage.visible
            width: 8
            height: 8
            radius: 4
            color: root.tone
        }

        Text {
            text: root.label
            color: root.tone
            font.pixelSize: Theme.typeCaption
            font.weight: Font.Medium
        }
    }
}
