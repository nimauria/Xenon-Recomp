import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property string title: "Confirm action"
    property string message: ""
    property string confirmText: "Confirm"
    property string cancelText: "Cancel"
    property bool destructive: false

    signal confirmed()

    parent: Overlay.overlay
    width: Math.min(560, parent ? parent.width - Theme.space2Xl * 2 : 560)
    implicitHeight: contentColumn.implicitHeight + Theme.spaceXl * 2
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape

    onOpened: {
        if (root.destructive)
            cancelButton.forceActiveFocus()
        else
            confirmButton.forceActiveFocus()
    }

    Overlay.modal: Rectangle { color: Theme.overlay }

    background: Rectangle {
        color: Theme.surfaceRaised
        radius: Theme.panelRadius
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

            Item { Layout.fillWidth: true }
            XButton {
                id: cancelButton
                automationId: "dialog-cancel"
                text: root.cancelText
                onClicked: root.close()
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
