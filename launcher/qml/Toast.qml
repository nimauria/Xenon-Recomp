import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property string titleText: ""
    property string messageText: ""

    x: parent ? parent.width - width - Theme.spaceXl : 0
    y: parent ? parent.height - height - 48 : 0
    width: Math.min(460, parent ? parent.width - Theme.spaceXl * 2 : 460)
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
        radius: Theme.panelRadius
        color: Theme.surfaceRaised
        border.width: Theme.borderWidth
        border.color: Theme.accent
    }

    contentItem: Item {
        // Popup itself is not an Item (it wraps one), so the Accessible
        // attached property belongs here, on the actual Item, rather than on
        // the Popup - attaching it to the Popup produced a real (if benign)
        // "must be attached to an object deriving from Item or Action"
        // warning on every launch.
        Accessible.role: Accessible.AlertMessage
        Accessible.name: root.titleText + ". " + root.messageText
        implicitHeight: column.implicitHeight + Theme.spaceXl

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: Theme.spaceMd
            spacing: Theme.spaceXs

            Text {
                Layout.fillWidth: true
                text: root.titleText
                color: Theme.text
                font.pixelSize: Theme.typeBody
                font.weight: Font.DemiBold
            }

            Text {
                Layout.fillWidth: true
                text: root.messageText
                color: Theme.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.typeCaption
                lineHeight: 1.25
            }
        }
    }

    Timer {
        id: closeTimer
        interval: launcherBridge.boolSetting("accessibility/reduceMotion", false) ? 5000 : 3800
        repeat: false
        onTriggered: root.close()
    }
}
