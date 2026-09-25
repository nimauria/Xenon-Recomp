import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: root

    property string title: ""
    property string description: ""
    default property alias content: body.data

    function runPageAction(actionId) {
        if (actionId === "scrollTop")
            root.contentItem.contentY = 0
        else if (actionId === "scrollBottom")
            root.contentItem.contentY = Math.max(0, root.contentItem.contentHeight - root.height)
    }

    clip: true
    contentWidth: availableWidth
    ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
    ScrollBar.vertical.policy: ScrollBar.AlwaysOff

    ColumnLayout {
        // x/width are both plain functions of root.availableWidth only - no
        // anchors.horizontalCenter against the ScrollView's internal
        // Flickable contentItem, which was creating a layout feedback loop
        // (the content settling over a few frames instead of landing
        // immediately, visible as pages "bouncing" into place).
        x: Math.max(0, (root.availableWidth - width) / 2)
        width: Math.min(root.availableWidth, Theme.contentMaxWidth)
        spacing: Theme.spaceMd

        XSectionHeader {
            Layout.fillWidth: true
            title: root.title
            description: root.description
            prominent: true
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
