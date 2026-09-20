import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    width: Math.min(420, root.parent ? root.parent.width - Theme.spaceLg * 2 : 420)
    padding: Theme.spaceLg
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        radius: Theme.panelRadius
        color: Theme.surface
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        Text {
            text: "Quick Center"
            color: Theme.text
            font.pixelSize: Theme.typeSubtitle
            font.weight: Font.DemiBold
        }

        Text {
            Layout.fillWidth: true
            text: "Handheld-friendly shortcuts for navigation and comfort settings."
            color: Theme.textMuted
            font.pixelSize: Theme.typeBody
            wrapMode: Text.WordWrap
        }

        GridLayout {
            Layout.fillWidth: true
            columns: width >= 360 ? 2 : 1
            columnSpacing: Theme.spaceSm
            rowSpacing: Theme.spaceSm

            XButton {
                Layout.fillWidth: true
                text: "Home"
                onClicked: { launcherBridge.executeCommandPaletteAction("navigate.home", "", ""); root.close() }
            }
            XButton {
                Layout.fillWidth: true
                text: "Library"
                onClicked: { launcherBridge.executeCommandPaletteAction("navigate.library", "", ""); root.close() }
            }
            XButton {
                Layout.fillWidth: true
                text: "Modules"
                onClicked: { launcherBridge.executeCommandPaletteAction("navigate.modules", "", ""); root.close() }
            }
            XButton {
                Layout.fillWidth: true
                text: "Settings"
                onClicked: { launcherBridge.executeCommandPaletteAction("navigate.settings", "", ""); root.close() }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        XSwitch {
            Layout.fillWidth: true
            text: "Reduce motion"
            accessibleName: "Reduce motion"
            checked: launcherBridge.boolSetting("accessibility/reduceMotion", false)
            onUserToggled: function(value) { launcherBridge.setSettingValue("accessibility/reduceMotion", value) }
        }

        XSwitch {
            Layout.fillWidth: true
            text: "Enhanced focus ring"
            accessibleName: "Enhanced focus ring"
            checked: launcherBridge.boolSetting("accessibility/enhancedFocus", false)
            onUserToggled: function(value) { launcherBridge.setSettingValue("accessibility/enhancedFocus", value) }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            StatusPill {
                label: launcherBridge.backendConnected ? "Runtime connected" : "Frontend ready"
                tone: launcherBridge.backendConnected ? Theme.success : Theme.textMuted
            }
            Item { Layout.fillWidth: true }
            XButton {
                text: "Close"
                variant: "ghost"
                onClicked: root.close()
            }
        }
    }
}
