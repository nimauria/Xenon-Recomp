import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    property int contextIndex: -1
    property int exportTargetIndex: -1
    property int deleteTargetIndex: -1
    readonly property bool testMode: launcherBridge.testMode
    readonly property var selected: ProfileStore.profile(ProfileStore.selectedIndex)

    function openContextMenuForFocusedItem() {
        if (ProfileStore.selectedIndex < 0 || ProfileStore.selectedIndex >= ProfileStore.profiles.length)
            return
        // profileActionsMenu.actions is a live binding to ProfileStore.selectedIndex
        // already - opening it is enough, assigning actions here would permanently
        // replace that binding with a static value.
        profileActionsMenu.open()
    }

    function movePage(forward) {
        var count = ProfileStore.profiles.length
        if (count === 0) return
        var step = 5
        var next = forward ? Math.min(count - 1, ProfileStore.selectedIndex + step)
                           : Math.max(0, ProfileStore.selectedIndex - step)
        ProfileStore.selectedIndex = next
        profileList.positionViewAtIndex(next, ListView.Contain)
    }

    function selectProfileById(profileId) {
        var target = String(profileId || "")
        if (target.length === 0) return false
        for (var i = 0; i < ProfileStore.profiles.length; ++i) {
            if (String(ProfileStore.profiles[i].profileId || "") === target) {
                ProfileStore.selectedIndex = i
                return true
            }
        }
        return false
    }

    function openCreateProfile() {
        profileEditor.openForCreate()
    }

    function runtimeSummary(profile) {
        var effective = profile.effectiveRuntimeSettings || ({})
        return {
            renderer: String(effective["runtime/graphicsBackend"] || "Automatic"),
            input: String(effective["input/preferredDevice"] || "Automatic"),
            volume: Math.round(Number(effective["audio/masterVolume"] === undefined ? 1.0 : effective["audio/masterVolume"]) * 100) + "%",
            cache: Boolean(effective["graphics/shaderCache"]) ? String(effective["graphics/shaderCacheMode"] || "Persistent") : "Disabled"
        }
    }

    function runProfileAction(actionId, index) {
        if (index < 0 || index >= ProfileStore.profiles.length)
            return
        ProfileStore.selectedIndex = index
        var profile = ProfileStore.profile(index)
        if (actionId === "activate") {
            if (ProfileStore.activate(index))
                launcherBridge.notify("Profile activated", profile.profileName + " is now active.")
        } else if (actionId === "edit") {
            profileEditor.openForEdit(index, profile, "profile")
        } else if (actionId === "runtime") {
            profileEditor.openForEdit(index, profile, "runtime")
        } else if (actionId === "paths") {
            profileEditor.openForEdit(index, profile, "paths")
        } else if (actionId === "duplicate") {
            ProfileStore.duplicate(index)
        } else if (actionId === "export") {
            exportTargetIndex = index
            exportProfileDialog.open()
        } else if (actionId === "copyId") {
            launcherBridge.copyText(String(profile.profileId || ""))
            launcherBridge.notify("Profile ID copied", String(profile.profileId || ""))
        } else if (actionId === "openStorage") {
            launcherBridge.openProfileStorage(index)
        } else if (actionId === "removeAvatar") {
            if (launcherBridge.removeProfileImage(index))
                ProfileStore.reloadBackend(index)
        } else if (actionId === "delete") {
            deleteTargetIndex = index
            deleteDialog.open()
        }
    }

    function openProfileContext(index, item, localX, localY) {
        if (index < 0 || index >= ProfileStore.profiles.length)
            return
        ProfileStore.selectedIndex = index
        contextIndex = index
        profileContextMenu.actions = launcherBridge.profileActions(index)
        profileContextMenu.openAt(item, localX, localY)
    }

    function openBackgroundContext(item, localX, localY) {
        contextIndex = -1
        profileBackgroundMenu.actions = launcherBridge.profileBackgroundActions()
        profileBackgroundMenu.openAt(item, localX, localY)
    }

    function runBackgroundAction(actionId) {
        if (actionId === "create")
            profileEditor.openForCreate()
        else if (actionId === "import")
            importProfileDialog.open()
    }

    function formatTimestamp(value, fallback) {
        if (!value || String(value).length === 0)
            return fallback
        var date = new Date(value)
        if (isNaN(date.getTime()))
            return String(value)
        return date.toLocaleString(Qt.locale(), Locale.ShortFormat)
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        XPanel {
            Layout.preferredWidth: Math.round(Math.min(390, 300 + Math.max(0, Theme.bodyScale - 1.0) * 120))
            Layout.minimumWidth: 280
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
                        elide: Text.ElideRight
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
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }

                    // This must live on the ListView viewport, not its scrolling contentItem.
                    // That makes blank-space context menus work even when the list is shorter than
                    // the visible viewport, and keeps hit testing correct after scrolling.
                    MouseArea {
                        parent: profileList
                        anchors.fill: profileList
                        z: 1000
                        acceptedButtons: Qt.RightButton
                        hoverEnabled: false
                        preventStealing: true
                        onClicked: function(mouse) {
                            var index = profileList.indexAt(
                                mouse.x + profileList.contentX,
                                mouse.y + profileList.contentY)
                            if (index >= 0)
                                root.openProfileContext(index, profileList, mouse.x, mouse.y)
                            else
                                root.openBackgroundContext(profileList, mouse.x, mouse.y)
                        }

                    }

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
                            anchors.fill: parent
                            anchors.margins: Theme.spaceMd
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
                                 : profileMouse.containsMouse ? Theme.surfaceHover : Theme.surface
                            border.width: profileDelegate.activeFocus ? Theme.focusWidth : Theme.borderWidth
                            border.color: profileDelegate.activeFocus ? Theme.focusRing
                                        : ProfileStore.selectedIndex === profileDelegate.index ? Theme.accent : Theme.border
                        }

                        onClicked: ProfileStore.selectedIndex = index

                        MouseArea {
                            id: profileMouse
                            anchors.fill: parent
                            acceptedButtons: Qt.LeftButton
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                ProfileStore.selectedIndex = profileDelegate.index
                                profileDelegate.forceActiveFocus()
                            }
                        }

                        Keys.onReturnPressed: ProfileStore.selectedIndex = index
                        Keys.onSpacePressed: ProfileStore.selectedIndex = index
                        onActiveFocusChanged: if (activeFocus) ProfileStore.selectedIndex = index
                    }

                    Text {
                        anchors.centerIn: profileList
                        // wrapMode only takes effect when the Text has an explicit width -
                        // without one it sizes to its unwrapped implicit width and
                        // anchors.centerIn then overflows both edges of the panel.
                        width: Math.min(implicitWidth, profileList.width - Theme.spaceLg * 2)
                        visible: ProfileStore.profiles.length === 0
                        text: "No local profiles yet.\nCreate one to manage your gamertag, gamerpic and game preferences."
                        color: Theme.textMuted
                        font.pixelSize: Theme.typeBody
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
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
                    id: profileHeaderPanel
                    Layout.fillWidth: true
                    implicitHeight: profileHeader.implicitHeight + Theme.spaceLg * 2
                    decorated: true

                    GridLayout {
                        id: profileHeader
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.spaceLg
                        columns: detailScroll.availableWidth >= 1040 && Theme.textScale <= 1.35 ? 2 : 1
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
                                        wrapMode: Text.NoWrap
                                        maximumLineCount: 1
                                        elide: Text.ElideRight
                                    }
                                    StatusPill {
                                        label: root.selected.active ? "Active" : "Inactive"
                                        tone: root.selected.active ? Theme.success : Theme.textMuted
                                    }
                                }
                                Text {
                                    Layout.fillWidth: true
                                    Layout.maximumWidth: 660
                                    text: root.selected.description || "No profile description."
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeBody
                                    wrapMode: Text.WordWrap
                                    maximumLineCount: 2
                                    elide: Text.ElideRight
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

                        RowLayout {
                            // No Layout.minimumWidth here - forcing a fixed floor wider
                            // than the buttons' own natural size made this row overflow
                            // whenever the window was narrower than that arbitrary
                            // number, regardless of what space was actually available.
                            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                            spacing: Theme.spaceSm

                            // Reserve the first action slot even for the active profile so
                            // Edit / Duplicate / More never shift when activation changes.
                            Item {
                                Layout.preferredWidth: 112
                                Layout.preferredHeight: Theme.controlHeight
                                XButton {
                                    anchors.fill: parent
                                    visible: !root.selected.active
                                    text: "Set Active"
                                    variant: "primary"
                                    onClicked: {
                                        ProfileStore.activate(ProfileStore.selectedIndex)
                                        launcherBridge.notify("Profile activated", root.selected.profileName + " is now active.")
                                    }
                                }
                            }

                            XButton {
                                Layout.preferredWidth: 126
                                text: "Edit Profile"
                                variant: root.selected.active ? "primary" : "default"
                                onClicked: profileEditor.openForEdit(ProfileStore.selectedIndex, root.selected)
                            }
                            XButton {
                                Layout.preferredWidth: 116
                                text: "Duplicate"
                                onClicked: ProfileStore.duplicate(ProfileStore.selectedIndex)
                            }
                            XIconButton {
                                id: profileActionsButton
                                Layout.preferredWidth: Theme.controlHeight
                                Layout.preferredHeight: Theme.controlHeight
                                iconName: "more"
                                tooltip: "More profile actions"
                                variant: "filled"
                                onClicked: profileActionsMenu.visible ? profileActionsMenu.close() : profileActionsMenu.open()
                                XActionMenu {
                                    id: profileActionsMenu
                                    parent: profileActionsButton
                                    x: profileActionsButton.width - width
                                    y: profileActionsButton.height + 4
                                    menuWidth: 250
                                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                    actions: launcherBridge.profileActions(ProfileStore.selectedIndex)
                                    onActionTriggered: function(actionId) {
                                        root.runProfileAction(actionId, ProfileStore.selectedIndex)
                                    }
                                }
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        z: 1000
                        acceptedButtons: Qt.RightButton
                        onClicked: function(mouse) {
                            root.openProfileContext(ProfileStore.selectedIndex, profileHeaderPanel, mouse.x, mouse.y)
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
                        Layout.alignment: Qt.AlignTop
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
                            XInfoRow { label: "Startup page"; value: root.selected.startupPage || "Launcher default" }
                            XInfoRow { label: "Offline mode"; value: root.selected.offline ? "Enabled" : "Disabled" }
                            XInfoRow { label: "Runtime settings"; value: root.selected.isolatedSettings ? "Profile-specific (" + root.selected.runtimeOverrideCount + " overrides)" : "Launcher defaults" }
                            XInfoRow { label: "Renderer"; value: root.runtimeSummary(root.selected).renderer }
                            XInfoRow { label: "Shader cache"; value: root.runtimeSummary(root.selected).cache }
                            XInfoRow { label: "Preferred input"; value: root.runtimeSummary(root.selected).input }
                            XInfoRow { label: "Audio volume"; value: root.runtimeSummary(root.selected).volume }
                        }
                    }

                    XPanel {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
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
                            XInfoRow { label: "Path overrides"; value: String(root.selected.pathOverrideCount || 0) }
                            XInfoRow { label: "Profile storage"; value: root.selected.storagePath || launcherBridge.stringSetting("paths/profiles", launcherBridge.defaultProfilesPath) }
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
                                ["Games", root.selected.effectiveGamePath || launcherBridge.defaultGameLibraryPath, root.selected.gamePath && root.selected.gamePath.length > 0 ? "Profile override" : "Launcher default"],
                                ["Saves", root.selected.effectiveSavePath || launcherBridge.defaultSaveDataPath, root.selected.savePath && root.selected.savePath.length > 0 ? "Profile override" : "Launcher default"],
                                ["Screenshots", root.selected.effectiveScreenshotPath || launcherBridge.defaultScreenshotsPath, root.selected.screenshotPath && root.selected.screenshotPath.length > 0 ? "Profile override" : "Launcher default"],
                                ["Profile storage", root.selected.storagePath || launcherBridge.stringSetting("paths/profiles", launcherBridge.defaultProfilesPath), "Managed profile folder"]
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
                                    enabled: launcherBridge.canOpenPath(modelData[1])
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

    XActionMenu {
        id: profileContextMenu
        parent: root
        menuWidth: 260
        actions: []
        onActionTriggered: function(actionId) {
            root.runProfileAction(actionId, root.contextIndex)
        }
    }

    XActionMenu {
        id: profileBackgroundMenu
        parent: root
        menuWidth: 240
        actions: []
        onActionTriggered: function(actionId) { root.runBackgroundAction(actionId) }
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

    FileDialog {
        id: exportProfileDialog
        title: "Export Xenon profile"
        fileMode: FileDialog.SaveFile
        nameFilters: ["Xenon profile (*.xenonprofile)"]
        onAccepted: {
            var target = root.exportTargetIndex >= 0 ? root.exportTargetIndex : ProfileStore.selectedIndex
            launcherBridge.exportProfile(target, selectedFile)
            root.exportTargetIndex = -1
        }
        onRejected: root.exportTargetIndex = -1
    }

    FileDialog {
        id: importProfileDialog
        title: "Import Xenon profile"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Xenon profile (*.xenonprofile)", "JSON files (*.json)"]
        onAccepted: {
            var importedIndex = launcherBridge.importProfile(selectedFile)
            if (importedIndex >= 0)
                ProfileStore.reloadBackend(importedIndex)
        }
    }

    XConfirmDialog {
        id: deleteDialog
        readonly property var targetProfile: ProfileStore.profile(root.deleteTargetIndex >= 0 ? root.deleteTargetIndex : ProfileStore.selectedIndex)
        title: "Delete “" + (targetProfile.profileName || "profile") + "”?"
        message: "This removes the local launcher profile configuration for this profile. Game content and globally installed modules are not deleted. This action cannot currently be undone."
        confirmText: "Delete profile"
        destructive: true
        onConfirmed: {
            var target = root.deleteTargetIndex >= 0 ? root.deleteTargetIndex : ProfileStore.selectedIndex
            var name = ProfileStore.profile(target).profileName
            if (ProfileStore.remove(target))
                launcherBridge.notify("Profile deleted", name + " was removed from the launcher.")
            root.deleteTargetIndex = -1
        }
        onClosed: root.deleteTargetIndex = -1
    }
}
