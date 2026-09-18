import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Popup {
    id: root

    property bool createMode: true
    // all = create flow, profile = identity/preferences, paths = content-location overrides
    property string editSection: "all"
    property int editingIndex: -1
    property string profileId: ""
    property string profileName: ""
    property string description: ""
    property string avatarPath: ""
    property string pendingAvatarSource: ""
    property bool removeAvatarOnSave: false
    property string gamePath: ""
    property string savePath: ""
    property string screenshotPath: ""
    property bool customLocations: false
    property string region: "Auto (Global)"
    property string startupPage: "Library"
    property bool offline: true
    property bool isolatedSettings: false
    property string baselineSnapshot: ""

    readonly property bool nameIsValid: nameField.text.trim().length > 0
        && ProfileStore.nameAvailable(nameField.text.trim(), root.editingIndex)
    readonly property bool dirty: root.currentSnapshot() !== root.baselineSnapshot

    signal submitted(var data)

    function currentSnapshot() {
        return JSON.stringify({
            profileName: nameField.text.trim(),
            description: descriptionField.text.trim(),
            avatarPath: avatarPath,
            pendingAvatarSource: pendingAvatarSource,
            removeAvatarOnSave: removeAvatarOnSave,
            customLocations: customLocations,
            gamePath: customLocations ? gamePath : "",
            savePath: customLocations ? savePath : "",
            screenshotPath: customLocations ? screenshotPath : "",
            region: region,
            startupPage: startupPage,
            offline: offline,
            isolatedSettings: isolatedSettings
        })
    }

    function captureBaseline() {
        baselineSnapshot = currentSnapshot()
    }

    function openForCreate() {
        createMode = true
        editSection = "all"
        editingIndex = -1
        profileId = ProfileStore.newProfileId()
        profileName = ""
        description = ""
        avatarPath = ""
        pendingAvatarSource = ""
        removeAvatarOnSave = false
        gamePath = ""
        savePath = ""
        screenshotPath = ""
        customLocations = false
        region = "Auto (Global)"
        startupPage = "Library"
        offline = true
        isolatedSettings = false
        nameField.text = ""
        descriptionField.text = ""
        validationText.visible = false
        open()
        Qt.callLater(function() {
            root.captureBaseline()
            nameField.forceActiveFocus()
        })
    }

    function openForEdit(index, data, section) {
        createMode = false
        editSection = section === "paths" ? "paths" : "profile"
        editingIndex = index
        profileId = String(data.profileId || "")
        profileName = String(data.profileName || "")
        description = String(data.description || "")
        avatarPath = String(data.avatarPath || "")
        pendingAvatarSource = ""
        removeAvatarOnSave = false
        gamePath = String(data.gamePath || "")
        savePath = String(data.savePath || "")
        screenshotPath = String(data.screenshotPath || "")
        customLocations = gamePath.length > 0 || savePath.length > 0 || screenshotPath.length > 0
        region = String(data.region || "Auto (Global)")
        startupPage = String(data.startupPage || "Library")
        offline = Boolean(data.offline)
        isolatedSettings = Boolean(data.isolatedSettings)
        nameField.text = profileName
        descriptionField.text = description
        validationText.visible = false
        open()
        Qt.callLater(function() {
            root.captureBaseline()
            if (root.editSection !== "paths")
                nameField.forceActiveFocus()
        })
    }

    function requestClose() {
        if (!dirty) {
            close()
            return
        }
        discardDialog.open()
    }

    function submit() {
        var trimmed = nameField.text.trim()
        if (trimmed.length === 0) {
            validationText.text = "Enter a profile name."
            validationText.visible = true
            nameField.forceActiveFocus()
            return
        }
        if (!ProfileStore.nameAvailable(trimmed, root.editingIndex)) {
            validationText.text = "A profile with this name already exists."
            validationText.visible = true
            nameField.forceActiveFocus()
            return
        }

        var finalAvatar = avatarPath
        if (removeAvatarOnSave) {
            launcherBridge.removeProfileAvatar(profileId)
            finalAvatar = ""
        } else if (pendingAvatarSource.length > 0) {
            var imported = launcherBridge.importProfileAvatar(profileId, pendingAvatarSource)
            if (imported.length > 0)
                finalAvatar = imported
        }

        validationText.visible = false
        submitted({
            profileId: profileId,
            profileName: trimmed,
            description: descriptionField.text.trim(),
            avatarPath: finalAvatar,
            gamePath: customLocations ? gamePath : "",
            savePath: customLocations ? savePath : "",
            screenshotPath: customLocations ? screenshotPath : "",
            region: region,
            startupPage: startupPage,
            offline: offline,
            isolatedSettings: isolatedSettings
        })
        baselineSnapshot = currentSnapshot()
        close()
    }

    parent: Overlay.overlay
    width: Math.min(860, parent ? parent.width - Theme.spaceLg * 2 : 860)
    height: Math.min(780, parent ? parent.height - Theme.spaceLg * 2 : 780)
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.NoAutoClose

    Shortcut { sequence: "Escape"; enabled: root.opened; onActivated: root.requestClose() }

    Overlay.modal: Rectangle {
        color: Theme.overlay
        MouseArea {
            anchors.fill: parent
            enabled: !discardDialog.opened
            onClicked: root.requestClose()
        }
    }

    background: Rectangle {
        color: Theme.surfaceRaised
        radius: Theme.panelRadius
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(82, Theme.typeSubtitle + Theme.typeCaption * 1.5 + Theme.spaceLg * 2)

            ColumnLayout {
                anchors.left: parent.left
                anchors.right: closeEditorButton.left
                anchors.leftMargin: Theme.spaceXl
                anchors.rightMargin: Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    Layout.fillWidth: true
                    text: root.createMode ? "Create profile" : (root.editSection === "paths" ? "Edit content locations" : "Edit profile")
                    color: Theme.text
                    font.pixelSize: Theme.typeSubtitle
                    font.weight: Font.DemiBold
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: root.createMode
                        ? "Create a local profile and optionally override launcher defaults."
                        : root.editSection === "paths"
                          ? "Choose profile-specific folders or inherit the launcher defaults."
                          : "Edit profile identity and per-profile defaults."
                    color: Theme.textMuted
                    font.pixelSize: Theme.typeCaption
                    wrapMode: Text.WordWrap
                }
            }

            XIconButton {
                id: closeEditorButton
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                iconName: "close"
                tooltip: "Close"
                variant: "ghost"
                onClicked: root.requestClose()
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        ScrollView {
            id: editorScroll
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            ScrollBar.vertical.policy: ScrollBar.AsNeeded

            Item {
                width: editorScroll.availableWidth
                implicitHeight: formColumn.implicitHeight + Theme.spaceLg * 2

                ColumnLayout {
                    id: formColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: Theme.spaceLg
                    spacing: Theme.spaceMd

                    GridLayout {
                        id: avatarLayout
                        visible: root.editSection !== "paths"
                        Layout.fillWidth: true
                        columns: width >= 560 && Theme.textScale < 1.5 ? 2 : 1
                        columnSpacing: Theme.spaceLg
                        rowSpacing: Theme.spaceMd

                        ProfileAvatar {
                            Layout.preferredWidth: Math.round(96 * Math.min(Theme.bodyScale, 1.25))
                            Layout.preferredHeight: Layout.preferredWidth
                            Layout.alignment: avatarLayout.columns === 1 ? Qt.AlignHCenter : Qt.AlignTop
                            displayName: nameField.text.length > 0 ? nameField.text : "Profile"
                            avatarSource: root.pendingAvatarSource.length > 0
                                ? root.pendingAvatarSource
                                : (root.removeAvatarOnSave ? "" : root.avatarPath)
                            editable: launcherBridge.featureEnabled("profiles.avatars")
                            onChangeRequested: avatarDialog.open()
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceSm
                            Text { text: "Profile image"; color: Theme.text; font.pixelSize: Theme.typeBody; font.weight: Font.DemiBold }
                            Text {
                                Layout.fillWidth: true
                                text: "Stored locally with this profile. PNG, JPEG and WebP are supported."
                                color: Theme.textMuted
                                wrapMode: Text.WordWrap
                                font.pixelSize: Theme.typeCaption
                            }
                            Flow {
                                visible: launcherBridge.featureEnabled("profiles.avatars")
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm
                                XButton { text: "Choose image"; onClicked: avatarDialog.open() }
                                XButton {
                                    visible: root.avatarPath.length > 0 || root.pendingAvatarSource.length > 0
                                    text: "Remove"
                                    variant: "ghost"
                                    onClicked: {
                                        root.pendingAvatarSource = ""
                                        root.removeAvatarOnSave = true
                                    }
                                }
                            }
                        }
                    }

                    ColumnLayout {
                        visible: root.editSection !== "paths"
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        Text { text: "Display name"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                        XTextField {
                            id: nameField
                            Layout.fillWidth: true
                            placeholderText: "Profile name"
                            accessibleName: "Profile name"
                            automationId: "profile-name"
                            onTextEdited: {
                                if (text.trim().length === 0) {
                                    validationText.text = "Enter a profile name."
                                    validationText.visible = true
                                } else if (!ProfileStore.nameAvailable(text.trim(), root.editingIndex)) {
                                    validationText.text = "A profile with this name already exists."
                                    validationText.visible = true
                                } else {
                                    validationText.visible = false
                                }
                            }
                        }
                        Text { id: validationText; visible: false; text: ""; color: Theme.danger; font.pixelSize: Theme.typeCaption }
                        Text {
                            visible: !validationText.visible
                            text: "The internal profile ID is generated automatically and never changes when the display name changes."
                            color: Theme.textMuted
                            font.pixelSize: Theme.typeCaption
                        }
                    }

                    ColumnLayout {
                        visible: root.editSection !== "paths"
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        Text { text: "Description"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                        XTextField { id: descriptionField; Layout.fillWidth: true; placeholderText: "Optional description"; accessibleName: "Profile description" }
                    }

                    GridLayout {
                        visible: root.editSection !== "paths"
                        Layout.fillWidth: true
                        columns: width > 560 ? 2 : 1
                        columnSpacing: Theme.spaceMd
                        rowSpacing: Theme.spaceMd
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceXs
                            Text { text: "Preferred region"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                            XComboBox {
                                Layout.fillWidth: true
                                model: ["Auto (Global)", "NTSC-U", "PAL", "NTSC-J"]
                                currentIndex: Math.max(0, model.indexOf(root.region))
                                onActivated: function(index) { root.region = model[index] }
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceXs
                            Text { text: "Startup page"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                            XComboBox {
                                Layout.fillWidth: true
                                model: ["Library", "Modules", "Profiles"]
                                currentIndex: Math.max(0, model.indexOf(root.startupPage))
                                onActivated: function(index) { root.startupPage = model[index] }
                            }
                        }
                    }

                    XSettingsCard {
                        visible: root.createMode || root.editSection === "paths"
                        title: "Use custom content locations"
                        description: "Leave this off to inherit Games, Saves and Screenshots from Settings → Paths."
                        actionWidth: 72
                        XSwitch {
                            checked: root.customLocations
                            onUserToggled: function(value) { root.customLocations = value }
                        }
                    }

                    ColumnLayout {
                        visible: (root.createMode || root.editSection === "paths") && root.customLocations
                        Layout.fillWidth: true
                        spacing: Theme.spaceMd
                        Text { text: "Content location overrides"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                        XPathField { label: "Game directory"; pathValue: root.gamePath; placeholderText: "Use launcher default"; allowClear: root.gamePath.length > 0; onPathEdited: function(path) { root.gamePath = path } }
                        XPathField { label: "Save data"; pathValue: root.savePath; placeholderText: "Use launcher default"; allowClear: root.savePath.length > 0; onPathEdited: function(path) { root.savePath = path } }
                        XPathField { label: "Screenshots"; pathValue: root.screenshotPath; placeholderText: "Use launcher default"; allowClear: root.screenshotPath.length > 0; onPathEdited: function(path) { root.screenshotPath = path } }
                    }

                    XSettingsCard {
                        visible: root.editSection !== "paths"
                        title: "Offline mode"
                        description: "Start games under this profile without online services by default."
                        actionWidth: 72
                        XSwitch { checked: root.offline; onUserToggled: function(value) { root.offline = value } }
                    }

                    XSettingsCard {
                        visible: root.editSection !== "paths"
                        title: "Profile-specific runtime settings"
                        description: "Keep future runtime overrides isolated from launcher-wide defaults."
                        actionWidth: 72
                        XSwitch { checked: root.isolatedSettings; onUserToggled: function(value) { root.isolatedSettings = value } }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(72, Theme.controlHeight + Theme.spaceLg * 2)

            Text {
                anchors.left: parent.left
                anchors.leftMargin: Theme.spaceXl
                anchors.verticalCenter: parent.verticalCenter
                visible: root.dirty
                text: "Unsaved changes"
                color: Theme.warning
                font.pixelSize: Theme.typeCaption
            }

            RowLayout {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceXl
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spaceSm
                XButton { text: "Cancel"; onClicked: root.requestClose() }
                XButton {
                    text: root.createMode ? "Create profile" : (root.editSection === "paths" ? "Save paths" : "Save changes")
                    variant: "primary"
                    enabled: root.editSection === "paths" ? true : root.nameIsValid
                    onClicked: root.submit()
                }
            }
        }
    }

    FileDialog {
        id: avatarDialog
        title: "Choose profile image"
        fileMode: FileDialog.OpenFile
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp)", "All files (*)"]
        onAccepted: {
            root.pendingAvatarSource = selectedFile
            root.removeAvatarOnSave = false
        }
    }

    XConfirmDialog {
        id: discardDialog
        title: root.createMode ? "Discard new profile?" : "Discard profile changes?"
        message: "Your unsaved changes will be lost."
        confirmText: "Discard changes"
        cancelText: "Continue editing"
        destructive: true
        onConfirmed: root.close()
    }
}
