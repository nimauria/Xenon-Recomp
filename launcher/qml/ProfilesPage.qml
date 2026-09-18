import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    readonly property bool testMode: launcherBridge.testMode
    readonly property var selected: ProfileStore.profile(ProfileStore.selectedIndex)

    function effectivePath(overridePath, key, fallback) {
        if (overridePath && overridePath.length > 0)
            return overridePath
        return launcherBridge.stringSetting(key, fallback)
    }

    function formatTimestamp(value, fallback) {
        if (!value || String(value).length === 0)
            return fallback
        var date = new Date(value)
        if (isNaN(date.getTime()))
            return String(value)
        return date.toLocaleString(Qt.locale(), Locale.ShortFormat)
    }

    function isOpenablePath(path) {
        return path && path.length > 0 && path.indexOf("TEST://") !== 0
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        XPanel {
            Layout.preferredWidth: 300
            Layout.minimumWidth: 270
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceSm

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        text: "Profiles"
                        color: Theme.text
                        font.pixelSize: Theme.typeSubtitle
                        font.weight: Font.DemiBold
                    }
                    StatusPill { visible: root.testMode; label: "TEST"; tone: Theme.warning }
                }

                Text {
                    Layout.fillWidth: true
                    text: "Local launcher identities, content locations and per-profile defaults."
                    color: Theme.textMuted
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.typeCaption
                }

                XButton {
                    Layout.fillWidth: true
                    automationId: "profiles-create"
                    text: "+  Create Profile"
                    variant: "primary"
                    onClicked: profileEditor.openForCreate()
                }

                ListView {
                    reuseItems: true
                    id: profileList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: Theme.spaceSm
                    model: ProfileStore.profiles
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Button {
                        id: profileDelegate
                        required property int index
                        required property var modelData
                        readonly property bool matchesSearch: root.searchText.trim().length === 0
                            || String(modelData.profileName || "").toLowerCase().indexOf(root.searchText.trim().toLowerCase()) !== -1
                            || String(modelData.description || "").toLowerCase().indexOf(root.searchText.trim().toLowerCase()) !== -1
                        width: profileList.width - (profileList.ScrollBar.vertical.visible ? 8 : 0)
                        height: matchesSearch ? Math.round(90 + Math.max(0, Theme.textScale - 1.0) * 44) : 0
                        visible: matchesSearch
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus
                        padding: 0
                        Accessible.name: modelData.profileName
                        Accessible.description: modelData.active ? "Active Xenon profile" : "Xenon profile"

                        contentItem: RowLayout {
                            anchors.margins: Theme.spaceSm
                            spacing: Theme.spaceSm

                            ProfileAvatar {
                                Layout.preferredWidth: 50
                                Layout.preferredHeight: 50
                                displayName: modelData.profileName
                                avatarSource: String(modelData.avatarPath || "")
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.profileName
                                    color: Theme.text
                                    font.pixelSize: Theme.typeBody
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: modelData.active ? "Active profile" : "Profile"
                                    color: modelData.active ? Theme.success : Theme.textMuted
                                    font.pixelSize: Theme.typeCaption
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.games + " games • " + modelData.saveSets + " save sets"
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeCaption
                                    elide: Text.ElideRight
                                }
                            }
                        }

                        background: Rectangle {
                            radius: Theme.controlRadius
                            color: ProfileStore.selectedIndex === profileDelegate.index ? Theme.accentSoft
                                 : profileDelegate.hovered ? Theme.surfaceHover : Theme.surface
                            border.width: profileDelegate.activeFocus ? Theme.focusWidth : Theme.borderWidth
                            border.color: profileDelegate.activeFocus ? Theme.focusRing
                                        : ProfileStore.selectedIndex === profileDelegate.index ? Theme.accent : Theme.border
                        }

                        onClicked: ProfileStore.selectedIndex = index
                    }
                }
            }
        }

        ScrollView {
            id: detailScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: detailScroll.availableWidth
                spacing: Theme.spaceMd

                XPanel {
                    Layout.fillWidth: true
                    implicitHeight: profileHeader.implicitHeight + Theme.spaceLg * 2

                    GridLayout {
                        id: profileHeader
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.spaceLg
                        columns: detailScroll.availableWidth >= 900 && Theme.textScale <= 1.35 ? 2 : 1
                        columnSpacing: Theme.spaceLg
                        rowSpacing: Theme.spaceMd

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceLg

                            ProfileAvatar {
                                Layout.preferredWidth: Math.round(84 * Math.min(Theme.textScale, 1.35))
                                Layout.preferredHeight: Math.round(84 * Math.min(Theme.textScale, 1.35))
                                displayName: root.selected.profileName || "Profile"
                                avatarSource: String(root.selected.avatarPath || "")
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceXs
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.selected.profileName || "Profile"
                                        color: Theme.text
                                        font.pixelSize: Theme.typeTitle
                                        font.weight: Font.DemiBold
                                        wrapMode: Text.WordWrap
                                    }
                                    StatusPill {
                                        label: root.selected.active ? "Active" : "Inactive"
                                        tone: root.selected.active ? Theme.success : Theme.textMuted
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.selected.description || "No profile description."
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeBody
                                    wrapMode: Text.WordWrap
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.selected.games + " games  •  " + root.selected.saveSets + " save sets  •  " + (root.selected.lastUsed || "Not used yet")
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeCaption
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }

                        Flow {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            spacing: Theme.spaceSm
                            XButton {
                                visible: !root.selected.active
                                text: "Set Active"
                                variant: "primary"
                                onClicked: {
                                    ProfileStore.activate(ProfileStore.selectedIndex)
                                    launcherBridge.notify("Profile activated", root.selected.profileName + " is now active.")
                                }
                            }
                            XButton {
                                text: "Edit Profile"
                                variant: root.selected.active ? "primary" : "default"
                                onClicked: profileEditor.openForEdit(ProfileStore.selectedIndex, root.selected)
                            }
                            XButton { text: "Duplicate"; onClicked: ProfileStore.duplicate(ProfileStore.selectedIndex) }
                            XIconButton {
                                id: profileActionsButton
                                iconName: "more"
                                tooltip: "More profile actions"
                                variant: "filled"
                                onClicked: profileActionsMenu.open()
                                XActionMenu {
                                    id: profileActionsMenu
                                    x: profileActionsButton.width - width
                                    y: profileActionsButton.height + 4
                                    menuWidth: 220
                                    actions: [
                                        { id: "export", label: "Export profile", icon: "⇧" },
                                        { id: "import", label: "Import profile", icon: "⇩" },
                                        { id: "delete", label: "Delete profile", icon: "×", destructive: true, separatorBefore: true }
                                    ]
                                    onActionTriggered: function(actionId) {
                                        if (actionId === "delete") {
                                            if (!root.selected.active && ProfileStore.profiles.length > 1)
                                                deleteDialog.open()
                                            else
                                                launcherBridge.notify("Profile protected", "The active profile cannot be deleted. Activate another profile first.")
                                        } else if (actionId === "export") {
                                            launcherBridge.notifyUnavailable("Export Profile")
                                        } else if (actionId === "import") {
                                            launcherBridge.notifyUnavailable("Import Profile")
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: detailScroll.availableWidth >= 930 && Theme.textScale <= 1.35 ? 2 : 1
                    columnSpacing: Theme.spaceMd
                    rowSpacing: Theme.spaceMd

                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: profileDetailsColumn.implicitHeight + Theme.spaceLg * 2
                        ColumnLayout {
                            id: profileDetailsColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Theme.spaceLg
                            spacing: Theme.spaceSm
                            XSectionHeader { title: "Profile details"; description: "Identity and effective launcher preferences. Use Edit Profile to change user-managed fields." }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                            XInfoRow {
                                label: "Profile ID"
                                value: root.selected.profileId || "Local profile"
                            }
                            XInfoRow { label: "Created"; value: root.formatTimestamp(root.selected.createdAt, "Unknown") }
                            XInfoRow { label: "Last activated"; value: root.formatTimestamp(root.selected.lastUsedAt, root.selected.lastUsed || "Not used yet") }
                            XInfoRow { label: "Preferred region"; value: root.selected.region || "Auto (Global)" }
                            XInfoRow { label: "Startup page"; value: root.selected.startupPage || "Library" }
                            XInfoRow { label: "Offline mode"; value: root.selected.offline ? "Enabled" : "Disabled" }
                            XInfoRow { label: "Runtime settings"; value: root.selected.isolatedSettings ? "Profile-specific" : "Launcher defaults" }
                        }
                    }

                    XPanel {
                        Layout.fillWidth: true
                        implicitHeight: profileSummaryColumn.implicitHeight + Theme.spaceLg * 2
                        ColumnLayout {
                            id: profileSummaryColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: Theme.spaceLg
                            spacing: Theme.spaceSm
                            XSectionHeader { title: "Profile summary"; description: "Content currently associated with this profile." }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                            XInfoRow { label: "Games"; value: String(root.selected.games || 0) }
                            XInfoRow { label: "Modules available"; value: String(root.selected.modules || 0) }
                            XInfoRow { label: "Save sets"; value: String(root.selected.saveSets || 0) }
                            XInfoRow { label: "Status"; value: root.selected.active ? "Active profile" : "Inactive" }
                            XInfoRow { label: "Profile storage"; value: launcherBridge.stringSetting("paths/profiles", launcherBridge.defaultProfilesPath) }
                            Text {
                                Layout.fillWidth: true
                                text: "Modules are installed globally. Profile-specific paths and runtime preferences can be edited independently."
                                color: Theme.textMuted
                                wrapMode: Text.WordWrap
                                font.pixelSize: Theme.typeCaption
                            }
                        }
                    }
                }

                XPanel {
                    Layout.fillWidth: true
                    implicitHeight: pathColumn.implicitHeight + Theme.spaceLg * 2

                    ColumnLayout {
                        id: pathColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.spaceLg
                        spacing: Theme.spaceSm

                        RowLayout {
                            Layout.fillWidth: true
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Content locations"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                                Text { Layout.fillWidth: true; text: "Overrides inherit Settings → Paths when no profile-specific folder is selected."; color: Theme.textMuted; font.pixelSize: Theme.typeCaption; wrapMode: Text.WordWrap }
                            }
                            XButton { text: "Edit Paths"; onClicked: profileEditor.openForEdit(ProfileStore.selectedIndex, root.selected, "paths") }
                        }
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                        Repeater {
                            model: [
                                ["Games", root.effectivePath(root.selected.gamePath, "paths/games", launcherBridge.defaultGameLibraryPath), root.selected.gamePath && root.selected.gamePath.length > 0 ? "Profile override" : "Launcher default"],
                                ["Saves", root.effectivePath(root.selected.savePath, "paths/saves", launcherBridge.defaultSaveDataPath), root.selected.savePath && root.selected.savePath.length > 0 ? "Profile override" : "Launcher default"],
                                ["Screenshots", root.effectivePath(root.selected.screenshotPath, "paths/screenshots", launcherBridge.defaultScreenshotsPath), root.selected.screenshotPath && root.selected.screenshotPath.length > 0 ? "Profile override" : "Launcher default"],
                                ["Profile storage", launcherBridge.stringSetting("paths/profiles", launcherBridge.defaultProfilesPath), "Launcher-wide"]
                            ]
                            delegate: GridLayout {
                                id: locationGrid
                                required property var modelData
                                Layout.fillWidth: true
                                columns: width >= 620 && Theme.textScale < 1.5 ? 3 : 1
                                columnSpacing: Theme.spaceMd
                                rowSpacing: Theme.spaceXs
                                Text { Layout.preferredWidth: locationGrid.columns === 3 ? 110 : -1; text: modelData[0]; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text { Layout.fillWidth: true; text: modelData[1]; color: Theme.text; font.pixelSize: Theme.typeBody; wrapMode: locationGrid.columns === 1 ? Text.WrapAnywhere : Text.NoWrap; elide: locationGrid.columns === 1 ? Text.ElideNone : Text.ElideMiddle }
                                    Text { text: modelData[2]; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                                }
                                XButton {
                                    Layout.fillWidth: locationGrid.columns === 1
                                    text: "Open"
                                    enabled: root.isOpenablePath(modelData[1])
                                    onClicked: launcherBridge.openFolder(modelData[1])
                                }
                            }
                        }
                    }
                }

                Item { Layout.preferredHeight: Theme.spaceXs }
            }
        }
    }

    ProfileEditorDialog {
        id: profileEditor
        onSubmitted: function(data) {
            if (profileEditor.createMode) {
                var newIndex = ProfileStore.createFromData(data)
                if (newIndex >= 0)
                    launcherBridge.notify("Profile created", ProfileStore.profile(newIndex).profileName + " was created.")
                else
                    launcherBridge.notify("Profile not created", "Choose a unique profile name and try again.")
            } else {
                if (ProfileStore.updateFromData(profileEditor.editingIndex, data))
                    launcherBridge.notify("Profile updated", data.profileName + " was updated.")
                else
                    launcherBridge.notify("Profile not updated", "Choose a unique profile name and try again.")
            }
        }
    }

    XConfirmDialog {
        id: deleteDialog
        title: "Delete “" + (root.selected.profileName || "profile") + "”?"
        message: "This removes the local launcher profile configuration for this profile. Game content and globally installed modules are not deleted. This action cannot currently be undone."
        confirmText: "Delete profile"
        destructive: true
        onConfirmed: {
            var name = root.selected.profileName
            var profileId = root.selected.profileId
            if (ProfileStore.remove(ProfileStore.selectedIndex)) {
                if (profileId && profileId.length > 0)
                    launcherBridge.removeProfileAvatar(profileId)
                launcherBridge.notify("Profile deleted", name + " was removed from the launcher.")
            }
        }
    }
}
