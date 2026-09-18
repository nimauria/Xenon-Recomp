import QtQuick
import QtQuick.Controls

Item {
    id: root

    property string displayName: "Profile"
    property string avatarSource: ""
    property bool editable: false
    signal changeRequested()
    signal removeRequested()

    implicitWidth: 96
    implicitHeight: 96

    Rectangle {
        anchors.fill: parent
        radius: Theme.panelRadius
        color: Theme.accentSoft
        border.width: Theme.borderWidth
        border.color: avatarMouse.containsMouse && root.editable ? Theme.accent : Theme.border
        clip: true

        Image {
            anchors.fill: parent
            source: root.avatarSource
            visible: root.avatarSource.length > 0
            fillMode: Image.PreserveAspectCrop
            asynchronous: true
            cache: true
            sourceSize.width: Math.max(128, width * 2)
            sourceSize.height: Math.max(128, height * 2)
        }

        Text {
            anchors.centerIn: parent
            visible: root.avatarSource.length === 0
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
