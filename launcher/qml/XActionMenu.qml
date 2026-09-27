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
    property int highlightedIndex: -1
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

    function isEnabled(index) {
        if (index < 0 || index >= root.actions.length) return false
        var item = root.actions[index]
        var visible = item.visible === undefined ? true : Boolean(item.visible)
        var enabled = item.enabled === undefined ? true : Boolean(item.enabled)
        return visible && enabled
    }

    function firstEnabledIndex() {
        for (var i = 0; i < root.actions.length; ++i) if (root.isEnabled(i)) return i
        return -1
    }

    function moveHighlight(forward) {
        if (root.actions.length === 0) return
        var start = root.highlightedIndex
        var index = start
        for (var step = 0; step < root.actions.length; ++step) {
            index = forward ? (index + 1) % root.actions.length
                            : (index - 1 + root.actions.length) % root.actions.length
            if (root.isEnabled(index)) { root.highlightedIndex = index; return }
            if (index === start) break
        }
    }

    function activateHighlighted() {
        if (root.isEnabled(root.highlightedIndex)) {
            var actionId = root.actions[root.highlightedIndex].id
            root.close()
            root.actionTriggered(actionId)
        }
    }

    width: menuWidth
    padding: Theme.spaceXs
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionFast } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast } }

    // Keeps this menu's Up/Down/Enter/Escape (keyboard) and
    // navigate/confirm/back (gamepad) entirely self-contained instead of
    // also moving focus in whatever page opened the menu - see
    // Main.qml's handleFrontendAction and NavigationGuard's own comment.
    onOpened: {
        root.highlightedIndex = root.firstEnabledIndex()
        NavigationGuard.pushModal()
        if (root.hasRequestedPoint)
            Qt.callLater(root.repositionRequestedPoint)
    }
    onClosed: {
        root.hasRequestedPoint = false
        root.highlightedIndex = -1
        NavigationGuard.popModal()
    }

    Connections {
        target: launcherBridge
        enabled: root.visible
        function onFrontendAction(action) {
            switch (action) {
            case "up": root.moveHighlight(false); break
            case "down": root.moveHighlight(true); break
            case "confirm": root.activateHighlighted(); break
            case "cancel": case "back": root.close(); break
            }
        }
    }

    Keys.onPressed: (event) => {
        switch (event.key) {
        case Qt.Key_Up: root.moveHighlight(false); event.accepted = true; break
        case Qt.Key_Down: root.moveHighlight(true); event.accepted = true; break
        case Qt.Key_Home: root.highlightedIndex = root.firstEnabledIndex(); event.accepted = true; break
        }
    }

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
                id: actionDelegate
                required property var modelData
                required property int index
                readonly property bool highlighted: root.highlightedIndex === index
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
                             : actionDelegate.highlighted ? Theme.surfaceHover
                             : "transparent"
                        // Keyboard/gamepad highlight is visually distinct from mouse
                        // hover (a filled border, not just the same hover tint) so
                        // the two input methods never look identical (Part 21).
                        border.width: (parent.activeFocus || actionDelegate.highlighted) ? Theme.focusWidth : 0
                        border.color: Theme.focusRing
                    }

                    onHoveredChanged: if (hovered) root.highlightedIndex = actionDelegate.index
                    onClicked: {
                        root.close()
                        root.actionTriggered(modelData.id)
                    }
                }
            }
        }
    }
}
