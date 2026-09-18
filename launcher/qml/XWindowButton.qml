import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control

    property string automationId: ""
    objectName: automationId

    // kind: minimize, maximize, close
    property string kind: "minimize"
    property bool maximized: false

    implicitWidth: kind === "close" ? 54 : 48
    implicitHeight: 44
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.TabFocus

    Accessible.name: kind === "minimize" ? "Minimize"
                   : kind === "maximize" ? (maximized ? "Restore" : "Maximize")
                   : "Close Xenon"

    contentItem: Item {
        anchors.fill: parent

        Rectangle {
            visible: control.kind === "minimize"
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: 3
            width: 15
            height: 1.5
            color: Theme.text
        }

        Rectangle {
            visible: control.kind === "maximize" && !control.maximized
            anchors.centerIn: parent
            width: 12
            height: 12
            color: "transparent"
            border.width: 1.4
            border.color: Theme.text
        }

        Item {
            visible: control.kind === "maximize" && control.maximized
            anchors.centerIn: parent
            width: 17
            height: 17

            Rectangle {
                width: 10
                height: 10
                x: 6
                y: 1
                color: Theme.header
                border.width: 1.25
                border.color: Theme.text
            }
            Rectangle {
                width: 10
                height: 10
                x: 1
                y: 6
                color: control.hovered ? Theme.surfaceHover : Theme.header
                border.width: 1.25
                border.color: Theme.text
            }
        }

        Item {
            visible: control.kind === "close"
            anchors.centerIn: parent
            width: 18
            height: 18
            Rectangle {
                anchors.centerIn: parent
                width: 17
                height: 1.6
                rotation: 45
                color: control.hovered ? "white" : Theme.text
            }
            Rectangle {
                anchors.centerIn: parent
                width: 17
                height: 1.6
                rotation: -45
                color: control.hovered ? "white" : Theme.text
            }
        }
    }

    background: Rectangle {
        color: control.kind === "close" && control.hovered ? Theme.danger
             : control.hovered ? Theme.surfaceHover
             : "transparent"
        border.width: 0
    }

    ToolTip.visible: control.hovered
    ToolTip.text: Accessible.name
    ToolTip.delay: 500
}
