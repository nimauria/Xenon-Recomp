import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Button {
    id: root

    implicitWidth: 206
    implicitHeight: Theme.controlHeight
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus

    readonly property var options: ProfileStore.names()
    readonly property var activeProfile: ProfileStore.profile(ProfileStore.activeIndex)

    Accessible.name: "Active profile " + (root.activeProfile.profileName || "Profile")
    Accessible.description: "Open profile switcher"

    contentItem: RowLayout {
        spacing: Theme.spaceSm

        ProfileAvatar {
            Layout.preferredWidth: 26
            Layout.preferredHeight: 26
            displayName: root.activeProfile.profileName || "Profile"
            avatarSource: String(root.activeProfile.avatarPath || "")
        }

        Text {
            Layout.fillWidth: true
            text: root.activeProfile.profileName || "Profile"
            color: Theme.text
            elide: Text.ElideRight
            font.pixelSize: Theme.typeBody
            font.weight: Font.DemiBold
        }

        Text {
            text: menu.visible ? "⌃" : "⌄"
            color: Theme.textMuted
            font.pixelSize: Theme.typeBody
        }
    }

    background: Rectangle {
        radius: Theme.controlRadius
        color: root.down ? Theme.surfaceRaised
             : menu.visible || root.hovered ? Theme.surfaceHover
             : Theme.input
        border.width: Theme.borderWidth
        border.color: root.activeFocus || menu.visible ? Theme.accent : Theme.border
    }

    onClicked: menu.visible ? menu.close() : menu.open()

    Popup {
        id: menu
        parent: root
        y: root.height + Theme.spaceXs
        x: root.width - width
        width: 260
        padding: Theme.spaceXs
        modal: false
        focus: true
        // Treat the profile button as the popup parent. Clicking it again is then
        // handled by the button's toggle instead of auto-closing on press and
        // immediately reopening on click.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        background: Rectangle {
            radius: Theme.controlRadius
            color: Theme.surfaceRaised
            border.width: Theme.borderWidth
            border.color: Theme.border
        }

        contentItem: ColumnLayout {
            spacing: 2

            Text {
                Layout.fillWidth: true
                text: "Switch profile"
                color: Theme.textMuted
                font.pixelSize: Theme.typeCaption
                font.weight: Font.DemiBold
                leftPadding: Theme.spaceSm
                topPadding: Theme.spaceXs
                bottomPadding: Theme.spaceXs
            }

            Repeater {
                model: root.options

                delegate: Button {
                    required property int index
                    required property string modelData
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(42, Theme.controlHeight)
                    hoverEnabled: true
                    focusPolicy: Qt.StrongFocus
                    Accessible.name: modelData + (index === ProfileStore.activeIndex ? ", active profile" : "")

                    contentItem: RowLayout {
                        spacing: Theme.spaceSm
                        ProfileAvatar {
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 24
                            displayName: modelData
                            avatarSource: String(ProfileStore.profile(index).avatarPath || "")
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData
                            color: Theme.text
                            elide: Text.ElideRight
                            font.pixelSize: Theme.typeBody
                        }
                        Text {
                            visible: index === ProfileStore.activeIndex
                            text: "✓"
                            color: Theme.success
                            font.pixelSize: Theme.typeBody
                            font.weight: Font.Bold
                        }
                    }

                    background: Rectangle {
                        radius: Theme.controlRadius
                        color: parent.down ? Theme.surfaceRaised
                             : parent.hovered ? Theme.surfaceHover
                             : index === ProfileStore.activeIndex ? Theme.accentSoft
                             : "transparent"
                        border.width: parent.activeFocus ? Theme.borderWidth : 0
                        border.color: Theme.accent
                    }

                    onClicked: {
                        if (index !== ProfileStore.activeIndex) {
                            ProfileStore.activate(index)
                            launcherBridge.notify("Profile activated", modelData + " is now the active profile.")
                        }
                        menu.close()
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                text: "Create and edit profiles from the Profiles page."
                color: Theme.textMuted
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.typeCaption
                leftPadding: Theme.spaceSm
                rightPadding: Theme.spaceSm
                bottomPadding: Theme.spaceXs
            }
        }
    }
}
