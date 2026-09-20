import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Flickable {
    id: root

    property string title: ""
    property string description: ""
    default property alias content: body.data

    function runPageAction(actionId) {
        if (actionId === "scrollTop")
            root.contentY = 0
        else if (actionId === "scrollBottom")
            root.contentY = Math.max(0, root.contentHeight - root.height)
    }

    clip: true
    contentWidth: width
    contentHeight: pageContent.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical.policy: ScrollBar.AsNeeded

    Item {
        id: pageContent
        width: root.width
        implicitHeight: pageColumn.implicitHeight + Theme.spaceXl * 2

        ColumnLayout {
            id: pageColumn
            width: Math.min(parent.width, Theme.contentMaxWidth)
            anchors.top: parent.top
            anchors.topMargin: Theme.spaceSm
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.spaceMd

            XSectionHeader {
                title: root.title
                description: root.description
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.divider
            }

            ColumnLayout {
                id: body
                Layout.fillWidth: true
                spacing: Theme.spaceSm
            }

            Item { Layout.preferredHeight: Theme.spaceLg }
        }
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        preventStealing: true
        z: 1000
        onClicked: function(mouse) { pageContextMenu.openAt(root, mouse.x, mouse.y) }
    }

    XActionMenu {
        id: pageContextMenu
        parent: root
        z: 1001
        menuWidth: 220
        actions: [
            { id: "scrollTop", label: "Scroll to top", icon: "↑" },
            { id: "scrollBottom", label: "Scroll to bottom", icon: "↓" }
        ]
        onActionTriggered: function(actionId) { root.runPageAction(actionId) }
    }
}
