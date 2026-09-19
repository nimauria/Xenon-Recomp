import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    width: 330
    padding: Theme.spaceLg
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

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
        XInfoRow { label: "Command palette"; value: "Ctrl+K" }
        XInfoRow { label: "Home"; value: "Ctrl+H" }
        XInfoRow { label: "Library"; value: "Ctrl+1" }
        XInfoRow { label: "Modules"; value: "Ctrl+2" }
        XInfoRow { label: "Profiles"; value: "Ctrl+3" }
        XInfoRow { label: "Settings"; value: "Ctrl+," }
        XInfoRow { label: "Search Xenon"; value: "Ctrl+F" }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
        Text { text: "Community & support"; color: Theme.text; font.pixelSize: Theme.typeBody; font.weight: Font.DemiBold }
        Text {
            Layout.fillWidth: true
            text: String(launcherBridge.communityInfo.supportText || "Join the Xenon community for support and development updates.")
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.typeCaption
            lineHeight: 1.25
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            XButton { Layout.fillWidth: true; text: "Join Discord"; variant: "primary"; onClicked: launcherBridge.openDiscordCommunity() }
            XButton { Layout.fillWidth: true; text: "GitHub"; onClicked: launcherBridge.openProjectCommunity() }
        }
        XButton {
            Layout.fillWidth: true
            text: "Create Support Bundle"
            onClicked: {
                var path = launcherBridge.createSupportBundle()
                if (String(path).length > 0) launcherBridge.openFolder(launcherBridge.supportBundleDirectory())
            }
        }
        Text {
            Layout.fillWidth: true
            text: "Creates a privacy-sanitized ZIP with launcher diagnostics, recent session results and bounded log excerpts. Review it before sharing."
            color: Theme.textMuted
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.typeCaption
            lineHeight: 1.25
        }
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
