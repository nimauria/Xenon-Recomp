import QtQuick
import QtQuick.Controls.Basic

Button {
    id: control

    property string automationId: ""
    objectName: automationId

    // kind: minimize, maximize, close
    property string kind: "minimize"
    property bool maximized: false

    implicitWidth: kind === "close" ? 48 : 44
    implicitHeight: 32
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
            anchors.verticalCenterOffset: 2
            width: 14
            height: 1.4
            color: Theme.text
        }

        Rectangle {
            visible: control.kind === "maximize" && !control.maximized
            anchors.centerIn: parent
            width: 11
            height: 11
            color: "transparent"
            border.width: 1.25
            border.color: Theme.text
        }

        Item {
            visible: control.kind === "maximize" && control.maximized
            anchors.centerIn: parent
            width: 15
            height: 15

            Rectangle {
                width: 9
                height: 9
                x: 5
                y: 1
                color: Theme.header
                border.width: 1.15
                border.color: Theme.text
            }
            Rectangle {
                width: 9
                height: 9
                x: 1
                y: 5
                color: control.hovered ? Theme.surfaceHover : Theme.header
                border.width: 1.15
                border.color: Theme.text
            }
        }

        Item {
            visible: control.kind === "close"
            anchors.centerIn: parent
            width: 15
            height: 15
            Rectangle {
                anchors.centerIn: parent
                width: 15
                height: 1.5
                rotation: 45
                color: control.hovered ? "white" : Theme.text
            }
            Rectangle {
                anchors.centerIn: parent
                width: 15
                height: 1.5
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
