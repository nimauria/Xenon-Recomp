import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property var actions: []
    property int menuWidth: 250
    property real requestedX: 0
    property real requestedY: 0
    property bool hasRequestedPoint: false
    signal actionTriggered(string actionId)

    function repositionRequestedPoint() {
        if (!root.parent)
            return
        var margin = Theme.spaceSm
        var popupWidth = root.width > 0 ? root.width : root.menuWidth
        var popupHeight = root.height > 0 ? root.height : root.implicitHeight
        root.x = Math.max(margin, Math.min(root.requestedX, root.parent.width - popupWidth - margin))
        root.y = Math.max(margin, Math.min(root.requestedY, root.parent.height - popupHeight - margin))
    }

    function openAt(anchorItem, localX, localY) {
        if (!anchorItem || !root.parent)
            return
        var point = anchorItem.mapToItem(root.parent, localX, localY)
        root.requestedX = point.x
        root.requestedY = point.y
        root.hasRequestedPoint = true
        root.open()
        Qt.callLater(root.repositionRequestedPoint)
    }

    width: menuWidth
    padding: Theme.spaceXs
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: {
        if (root.hasRequestedPoint)
            Qt.callLater(root.repositionRequestedPoint)
    }
    onClosed: root.hasRequestedPoint = false

    background: Rectangle {
        radius: Theme.controlRadius
        color: Theme.surface
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 2

        Repeater {
            model: root.actions

            delegate: ColumnLayout {
                required property var modelData
                visible: modelData.visible === undefined ? true : Boolean(modelData.visible)
                Layout.fillWidth: true
                spacing: 2

                Rectangle {
                    visible: Boolean(modelData.separatorBefore)
                    Layout.fillWidth: true
                    Layout.preferredHeight: visible ? 1 : 0
                    color: Theme.divider
                }

                Button {
                    id: actionButton
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.controlHeight
                    hoverEnabled: true
                    enabled: modelData.enabled === undefined ? true : Boolean(modelData.enabled)
                    focusPolicy: Qt.StrongFocus
                    Accessible.name: modelData.label
                    Accessible.description: enabled ? "" : String(modelData.disabledReason || "Unavailable")
                    ToolTip.visible: hovered && !enabled && String(modelData.disabledReason || "").length > 0
                    ToolTip.text: String(modelData.disabledReason || "")

                    contentItem: RowLayout {
                        spacing: Theme.spaceSm

                        Text {
                            Layout.preferredWidth: 20
                            text: modelData.icon || ""
                            color: !actionButton.enabled ? Theme.textMuted : modelData.destructive ? Theme.danger : Theme.textMuted
                            font.pixelSize: Theme.typeBody
                            horizontalAlignment: Text.AlignHCenter
                        }

                        Text {
                            Layout.fillWidth: true
                            text: modelData.label
                            color: !actionButton.enabled ? Theme.textMuted : modelData.destructive ? Theme.danger : Theme.text
                            font.pixelSize: Theme.typeBody
                            elide: Text.ElideRight
                        }
                    }

                    background: Rectangle {
                        radius: Theme.controlRadius
                        color: parent.down ? Theme.surfaceRaised
                             : parent.hovered ? Theme.surfaceHover
                             : "transparent"
                        border.width: parent.activeFocus ? Theme.borderWidth : 0
                        border.color: Theme.accent
                    }

                    onClicked: {
                        root.close()
                        root.actionTriggered(modelData.id)
                    }
                }
            }
        }
    }
}
