import QtQuick
import QtQuick.Controls

Popup {
    id: root

    property string titleText: ""
    property string messageText: ""

    x: parent ? parent.width - width - 24 : 0
    y: parent ? parent.height - height - 48 : 0
    width: Math.min(460, parent ? parent.width - 48 : 460)
    padding: 0
    modal: false
    focus: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    function show(title, message) {
        titleText = title
        messageText = message
        open()
        closeTimer.restart()
    }

    background: Rectangle {
        radius: 10
        color: Theme.surface
        border.width: 1
        border.color: Theme.accent
    }

    contentItem: Item {
        implicitHeight: column.implicitHeight + 28

        Column {
            id: column
            anchors.fill: parent
            anchors.margins: 14
            spacing: 6

            Text {
                width: parent.width
                text: root.titleText
                color: Theme.text
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }

            Text {
                width: parent.width
                text: root.messageText
                color: Theme.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                lineHeight: 1.2
            }
        }
    }

    Timer {
        id: closeTimer
        interval: 3600
        repeat: false
        onTriggered: root.close()
    }
}
