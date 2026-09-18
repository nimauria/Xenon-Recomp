import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root

    property string title: ""
    property string description: ""
    default property alias content: body.data

    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

    Item {
        width: root.availableWidth
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
}
