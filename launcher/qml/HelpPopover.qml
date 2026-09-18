import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    width: 330
    padding: Theme.spaceLg
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.surfaceRaised
        radius: Theme.panelRadius
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceSm

        Text { text: "Keyboard shortcuts"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
        XInfoRow { label: "Search current page"; value: "Ctrl+K" }
        XInfoRow { label: "Library"; value: "Ctrl+1" }
        XInfoRow { label: "Modules"; value: "Ctrl+2" }
        XInfoRow { label: "Profiles"; value: "Ctrl+3" }
        XInfoRow { label: "Settings"; value: "Ctrl+," }
        XInfoRow { label: "Find settings"; value: "Ctrl+F" }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
        Text {
            Layout.fillWidth: true
            text: "Game files and add-ons are always supplied locally by the user. Xenon modules never include copyrighted game content."
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.typeCaption
            lineHeight: 1.25
        }
    }
}
