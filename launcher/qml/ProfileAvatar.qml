import QtQuick
import QtQuick.Controls

Item {
    id: root

    property string displayName: "Profile"
    property string avatarSource: ""
    property real avatarFocalX: 0.5
    property real avatarFocalY: 0.5
    property real avatarZoom: 1.0
    property bool editable: false
    signal changeRequested()
    signal removeRequested()
    readonly property bool imageReady: avatarImage.imageReady

    implicitWidth: 96
    implicitHeight: 96

    Rectangle {
        anchors.fill: parent
        radius: Theme.panelRadius
        color: Theme.accentSoft
        border.width: Theme.borderWidth
        border.color: avatarMouse.containsMouse && root.editable ? Theme.accent : Theme.border
        clip: true

        CoverImage {
            id: avatarImage
            anchors.fill: parent
            source: root.avatarSource
            visible: imageReady
            fitMode: "cover"
            focalX: root.avatarFocalX
            focalY: root.avatarFocalY
            zoom: root.avatarZoom
            decodeHeadroom: 4.0
        }

        Text {
            anchors.centerIn: parent
            visible: !root.imageReady
            text: root.displayName.length > 0 ? root.displayName.charAt(0).toUpperCase() : "?"
            color: Theme.accent
            font.pixelSize: Math.min(root.width, root.height) * 0.38
            font.weight: Font.DemiBold
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 30
            visible: root.editable && avatarMouse.containsMouse
            color: Theme.overlay

            Text {
                anchors.centerIn: parent
                text: "Change image"
                color: Theme.text
                font.pixelSize: Theme.typeCaption
                font.weight: Font.DemiBold
            }
        }
    }

    MouseArea {
        id: avatarMouse
        anchors.fill: parent
        enabled: root.editable
        hoverEnabled: true
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.changeRequested()
    }
}
