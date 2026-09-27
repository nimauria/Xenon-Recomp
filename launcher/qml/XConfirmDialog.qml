import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property string title: "Confirm action"
    property string message: ""
    property string confirmText: "Confirm"
    property string cancelText: "Cancel"
    property string secondaryText: ""
    property bool destructive: false
    property bool secondaryDestructive: false

    signal confirmed()
    signal secondaryTriggered()
    signal cancelled()

    parent: Overlay.overlay
    width: Math.min(560, parent ? parent.width - Theme.space2Xl * 2 : 560)
    implicitHeight: contentColumn.implicitHeight + Theme.spaceXl * 2
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: {
        NavigationGuard.pushModal()
        if (root.destructive)
            cancelButton.forceActiveFocus()
        else
            confirmButton.forceActiveFocus()
    }
    onClosed: NavigationGuard.popModal()

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionNormal } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast } }

    Connections {
        target: launcherBridge
        enabled: root.visible
        function onFrontendAction(action) {
            if (action === "left" || action === "up")
                cancelButton.forceActiveFocus()
            else if (action === "right" || action === "down")
                confirmButton.forceActiveFocus()
            else if (action === "confirm") {
                if (cancelButton.activeFocus)
                    cancelButton.click()
                else if (secondaryButton.visible && secondaryButton.activeFocus)
                    secondaryButton.click()
                else
                    confirmButton.click()
            } else if (action === "cancel" || action === "back") {
                root.close()
                root.cancelled()
            }
        }
    }

    Overlay.modal: Rectangle { color: Theme.overlay }

    background: Rectangle {
        color: Theme.surfaceRaised
        radius: Theme.dialogRadius
        border.width: Theme.borderWidth
        border.color: root.destructive ? Theme.danger : Theme.border
    }

    contentItem: ColumnLayout {
        id: contentColumn
        spacing: Theme.spaceLg

        Item { Layout.preferredHeight: Theme.spaceXs }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            spacing: Theme.spaceMd

            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: 17
                color: root.destructive ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.16)
                                        : Theme.accentSoft
                Text {
                    anchors.centerIn: parent
                    text: root.destructive ? "!" : "?"
                    color: root.destructive ? Theme.danger : Theme.accent
                    font.pixelSize: Theme.typeBodyLarge
                    font.weight: Font.Bold
                }
            }

            Text {
                Layout.fillWidth: true
                text: root.title
                color: Theme.text
                font.pixelSize: Theme.typeSubtitle
                font.weight: Font.DemiBold
                wrapMode: Text.WordWrap
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            text: root.message
            color: Theme.textMuted
            font.pixelSize: Theme.typeBody
            lineHeight: 1.3
            wrapMode: Text.WordWrap
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            Layout.bottomMargin: Theme.spaceLg
            spacing: Theme.spaceSm

            XButton {
                id: secondaryButton
                visible: root.secondaryText.length > 0
                text: root.secondaryText
                variant: root.secondaryDestructive ? "danger" : "ghost"
                onClicked: {
                    root.close()
                    root.secondaryTriggered()
                }
            }

            Item { Layout.fillWidth: true }
            XButton {
                id: cancelButton
                automationId: "dialog-cancel"
                text: root.cancelText
                onClicked: {
                    root.close()
                    root.cancelled()
                }
            }
            XButton {
                id: confirmButton
                automationId: "dialog-confirm"
                text: root.confirmText
                variant: root.destructive ? "danger" : "primary"
                onClicked: {
                    root.close()
                    root.confirmed()
                }
            }
        }
    }
}
