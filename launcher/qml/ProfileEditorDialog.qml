import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Popup {
    id: root

    property bool createMode: true
    // all = create flow, profile = identity/preferences, runtime = per-profile runtime overrides, paths = content-location overrides
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
    property string startupPage: "Launcher default"
    property bool offline: true
    property bool isolatedSettings: false
    property var runtimeOverrides: ({})
    readonly property var runtimeDefinitions: launcherBridge.profileRuntimeDefinitions()
    property string baselineSnapshot: ""
    property bool allowDirtyClose: false
    property bool reopenForUnsavedPrompt: false

    readonly property bool nameIsValid: nameField.text.trim().length > 0
        && ProfileStore.nameAvailable(nameField.text.trim(), root.editingIndex)
    readonly property int descriptionLimit: ProfileStore.descriptionLimit
    readonly property bool dirty: root.currentSnapshot() !== root.baselineSnapshot

    signal submitted(var data)

    function runtimeDefinition(key) {
        for (var i = 0; i < runtimeDefinitions.length; ++i) {
            if (String(runtimeDefinitions[i].key) === String(key))
                return runtimeDefinitions[i]
        }
        return ({ key: key, title: key, description: "", options: [], effectiveDefault: "" })
    }

    function runtimeOptionLabels(key) {
        var options = runtimeDefinition(key).options || []
        var labels = []
        for (var i = 0; i < options.length; ++i)
            labels.push(String(options[i].label))
        return labels
    }

    function runtimeOptionValues(key) {
        var options = runtimeDefinition(key).options || []
        var values = []
        for (var i = 0; i < options.length; ++i)
            values.push(options[i].value)
        return values
    }

    function runtimeValue(key) {
        if (runtimeOverrides && runtimeOverrides[key] !== undefined)
            return runtimeOverrides[key]
        return runtimeDefinition(key).effectiveDefault
    }

    function setRuntimeValue(key, value) {
        var updated = {}
        if (runtimeOverrides) {
            for (var existing in runtimeOverrides)
                updated[existing] = runtimeOverrides[existing]
        }
        updated[key] = value
        runtimeOverrides = updated
    }

    function runtimeOptionIndex(key) {
        var values = runtimeOptionValues(key)
        var current = runtimeValue(key)
        for (var i = 0; i < values.length; ++i) {
            if (String(values[i]) === String(current) || Number(values[i]) === Number(current))
                return i
        }
        return 0
    }

    function setRuntimeOption(key, index) {
        var values = runtimeOptionValues(key)
        if (index >= 0 && index < values.length)
            setRuntimeValue(key, values[index])
    }

    function ensureRuntimeOverrides() {
        var defaults = launcherBridge.profileRuntimeDefaults()
        var updated = {}
        for (var key in defaults)
            updated[key] = runtimeOverrides && runtimeOverrides[key] !== undefined ? runtimeOverrides[key] : defaults[key]
        runtimeOverrides = updated
    }

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
            isolatedSettings: isolatedSettings,
            runtimeOverrides: isolatedSettings ? runtimeOverrides : ({})
        })
    }

    function captureBaseline() {
        baselineSnapshot = currentSnapshot()
    }

    function openForCreate() {
        createMode = true
        editSection = "all"
        editingIndex = -1
        profileId = ""
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
        startupPage = "Launcher default"
        offline = true
        isolatedSettings = false
        runtimeOverrides = launcherBridge.profileRuntimeDefaults()
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
        editSection = section === "paths" ? "paths" : (section === "runtime" ? "runtime" : "profile")
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
        startupPage = String(data.startupPage || "Launcher default")
        offline = Boolean(data.offline)
        isolatedSettings = Boolean(data.isolatedSettings)
        runtimeOverrides = data.runtimeOverrides || ({})
        if (isolatedSettings)
            ensureRuntimeOverrides()
        nameField.text = profileName
        descriptionField.text = description
        validationText.visible = false
        open()
        Qt.callLater(function() {
            root.captureBaseline()
            if (root.editSection === "profile")
                nameField.forceActiveFocus()
        })
    }

    function requestClose() {
        if (!dirty) {
            close()
            return
        }
        unsavedDialog.open()
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

        validationText.visible = false
        submitted({
            profileId: profileId,
            profileName: trimmed,
            description: descriptionField.text.trim(),
            avatarPath: avatarPath,
            pendingAvatarSource: pendingAvatarSource,
            removeAvatarOnSave: removeAvatarOnSave,
            gamePath: customLocations ? gamePath : "",
            savePath: customLocations ? savePath : "",
            screenshotPath: customLocations ? screenshotPath : "",
            region: region,
            startupPage: startupPage,
            offline: offline,
            isolatedSettings: isolatedSettings,
            runtimeOverrides: isolatedSettings ? runtimeOverrides : ({})
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
    // Let Qt detect clicks outside the modal reliably. If the form is dirty,
    // the popup is restored immediately and Xenon's unsaved-changes prompt
    // is shown over it. This avoids platform-specific overlay hit-test issues.
    closePolicy: Popup.CloseOnPressOutside

    onAboutToHide: {
        if (root.dirty && !root.allowDirtyClose && !unsavedDialog.opened)
            root.reopenForUnsavedPrompt = true
    }

    onClosed: {
        if (root.reopenForUnsavedPrompt) {
            root.reopenForUnsavedPrompt = false
            Qt.callLater(function() {
                root.open()
                Qt.callLater(function() { unsavedDialog.open() })
            })
        }
        root.allowDirtyClose = false
    }

    Shortcut {
        sequence: "Escape"
        enabled: root.opened && !unsavedDialog.opened
        onActivated: root.requestClose()
    }

    // Enter is the default action while creating a profile, regardless of
    // which normal form control currently has focus.
    Shortcut {
        sequence: "Return"
        context: Qt.ApplicationShortcut
        enabled: root.opened
            && root.createMode
            && root.nameIsValid
            && !unsavedDialog.opened
            && !avatarDialog.visible
        onActivated: root.submit()
    }
    Shortcut {
        sequence: "Enter"
        context: Qt.ApplicationShortcut
        enabled: root.opened
            && root.createMode
            && root.nameIsValid
            && !unsavedDialog.opened
            && !avatarDialog.visible
        onActivated: root.submit()
    }

    Overlay.modal: Rectangle { color: Theme.overlay }

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
                    text: root.createMode ? "Create profile"
                        : root.editSection === "paths" ? "Edit content locations"
                        : root.editSection === "runtime" ? "Profile runtime settings"
                        : "Edit profile"
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
                          : root.editSection === "runtime"
                            ? "Override launcher runtime, graphics, input and audio defaults for this profile."
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
                        visible: root.editSection !== "paths" && root.editSection !== "runtime"
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
                        visible: root.editSection !== "paths" && root.editSection !== "runtime"
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        Text { text: "Display name"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                        XTextField {
                            id: nameField
                            Layout.fillWidth: true
                            placeholderText: "Profile name"
                            accessibleName: "Profile name"
                            automationId: "profile-name"
                            maximumLength: ProfileStore.profileNameLimit
                            onAccepted: {
                                if (root.createMode && root.nameIsValid && !unsavedDialog.opened)
                                    root.submit()
                            }
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
                            text: "Profile IDs are generated automatically and remain stable when the display name changes."
                            color: Theme.textMuted
                            font.pixelSize: Theme.typeCaption
                        }
                    }

                    ColumnLayout {
                        visible: root.editSection !== "paths" && root.editSection !== "runtime"
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        RowLayout {
                            Layout.fillWidth: true
                            Text { text: "Description"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: descriptionField.text.length + " / " + root.descriptionLimit
                                color: descriptionField.text.length >= root.descriptionLimit ? Theme.warning : Theme.textMuted
                                font.pixelSize: Theme.typeCaption
                            }
                        }
                        XTextField {
                            id: descriptionField
                            Layout.fillWidth: true
                            placeholderText: "Optional description"
                            accessibleName: "Profile description"
                            maximumLength: root.descriptionLimit
                            onAccepted: {
                                if (root.createMode && root.nameIsValid && !unsavedDialog.opened)
                                    root.submit()
                            }
                        }
                    }

                    GridLayout {
                        visible: root.editSection !== "paths" && root.editSection !== "runtime"
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
                                model: ["Launcher default", "Home", "Library", "Modules", "Profiles", "Settings"]
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
                        description: root.isolatedSettings
                            ? "This profile uses its own runtime, graphics, input and audio defaults."
                            : "Inherit runtime, graphics, input and audio defaults from Settings."
                        actionWidth: 72
                        XSwitch {
                            checked: root.isolatedSettings
                            onUserToggled: function(value) {
                                root.isolatedSettings = value
                                if (value)
                                    root.ensureRuntimeOverrides()
                            }
                        }
                    }

                    ColumnLayout {
                        visible: root.isolatedSettings && (root.createMode || root.editSection === "runtime")
                        Layout.fillWidth: true
                        spacing: Theme.spaceSm

                        RowLayout {
                            Layout.fillWidth: true
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text { text: "Runtime overrides"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                                Text {
                                    Layout.fillWidth: true
                                    text: "These values are validated by Launcher Core and replace the matching launcher-wide defaults only for this profile."
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeCaption
                                    wrapMode: Text.WordWrap
                                }
                            }
                            XButton {
                                text: "Use current defaults"
                                variant: "ghost"
                                onClicked: root.runtimeOverrides = launcherBridge.profileRuntimeDefaults()
                            }
                        }

                        XSettingsCard {
                            title: root.runtimeDefinition("runtime/graphicsBackend").title
                            description: root.runtimeDefinition("runtime/graphicsBackend").description
                            actionWidth: 230
                            XComboBox {
                                Layout.preferredWidth: 220
                                model: root.runtimeOptionLabels("runtime/graphicsBackend")
                                currentIndex: root.runtimeOptionIndex("runtime/graphicsBackend")
                                onActivated: function(index) { root.setRuntimeOption("runtime/graphicsBackend", index) }
                            }
                        }
                        XSettingsCard {
                            title: root.runtimeDefinition("graphics/shaderCache").title
                            description: root.runtimeDefinition("graphics/shaderCache").description
                            actionWidth: 72
                            XSwitch {
                                checked: Boolean(root.runtimeValue("graphics/shaderCache"))
                                onUserToggled: function(value) { root.setRuntimeValue("graphics/shaderCache", value) }
                            }
                        }
                        XSettingsCard {
                            title: root.runtimeDefinition("graphics/shaderCacheMode").title
                            description: root.runtimeDefinition("graphics/shaderCacheMode").description
                            enabled: Boolean(root.runtimeValue("graphics/shaderCache"))
                            actionWidth: 230
                            XComboBox {
                                Layout.preferredWidth: 220
                                model: root.runtimeOptionLabels("graphics/shaderCacheMode")
                                currentIndex: root.runtimeOptionIndex("graphics/shaderCacheMode")
                                onActivated: function(index) { root.setRuntimeOption("graphics/shaderCacheMode", index) }
                            }
                        }
                        XSettingsCard {
                            title: root.runtimeDefinition("input/preferredDevice").title
                            description: root.runtimeDefinition("input/preferredDevice").description
                            actionWidth: 230
                            XComboBox {
                                Layout.preferredWidth: 220
                                model: root.runtimeOptionLabels("input/preferredDevice")
                                currentIndex: root.runtimeOptionIndex("input/preferredDevice")
                                onActivated: function(index) { root.setRuntimeOption("input/preferredDevice", index) }
                            }
                        }
                        XSettingsCard {
                            title: root.runtimeDefinition("input/deadzone").title
                            description: root.runtimeDefinition("input/deadzone").description
                            actionWidth: 230
                            XComboBox {
                                Layout.preferredWidth: 220
                                model: root.runtimeOptionLabels("input/deadzone")
                                currentIndex: root.runtimeOptionIndex("input/deadzone")
                                onActivated: function(index) { root.setRuntimeOption("input/deadzone", index) }
                            }
                        }
                        XSettingsCard {
                            title: root.runtimeDefinition("input/rumble").title
                            description: root.runtimeDefinition("input/rumble").description
                            actionWidth: 72
                            XSwitch {
                                checked: Boolean(root.runtimeValue("input/rumble"))
                                onUserToggled: function(value) { root.setRuntimeValue("input/rumble", value) }
                            }
                        }
                        XSettingsCard {
                            title: root.runtimeDefinition("audio/masterVolume").title
                            description: root.runtimeDefinition("audio/masterVolume").description
                            actionWidth: 230
                            XComboBox {
                                Layout.preferredWidth: 220
                                model: root.runtimeOptionLabels("audio/masterVolume")
                                currentIndex: root.runtimeOptionIndex("audio/masterVolume")
                                onActivated: function(index) { root.setRuntimeOption("audio/masterVolume", index) }
                            }
                        }
                        XSettingsCard {
                            title: root.runtimeDefinition("audio/muteUnfocused").title
                            description: root.runtimeDefinition("audio/muteUnfocused").description
                            actionWidth: 72
                            XSwitch {
                                checked: Boolean(root.runtimeValue("audio/muteUnfocused"))
                                onUserToggled: function(value) { root.setRuntimeValue("audio/muteUnfocused", value) }
                            }
                        }
                        XSettingsCard {
                            title: root.runtimeDefinition("audio/latencyProfile").title
                            description: root.runtimeDefinition("audio/latencyProfile").description
                            actionWidth: 230
                            XComboBox {
                                Layout.preferredWidth: 220
                                model: root.runtimeOptionLabels("audio/latencyProfile")
                                currentIndex: root.runtimeOptionIndex("audio/latencyProfile")
                                onActivated: function(index) { root.setRuntimeOption("audio/latencyProfile", index) }
                            }
                        }
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
                    text: root.createMode ? "Create profile"
                        : root.editSection === "paths" ? "Save paths"
                        : root.editSection === "runtime" ? "Save runtime settings"
                        : "Save changes"
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
        id: unsavedDialog
        title: root.createMode ? "Save this profile?" : "Save profile changes?"
        message: root.createMode
            ? "This profile has unsaved details. Save it before closing?"
            : "You have unsaved profile changes. Save them before closing?"
        confirmText: root.createMode ? "Create profile"
            : root.editSection === "paths" ? "Save paths"
            : root.editSection === "runtime" ? "Save runtime settings"
            : "Save changes"
        cancelText: "Cancel"
        secondaryText: "Discard changes"
        secondaryDestructive: true
        destructive: false
        onConfirmed: root.submit()
        onSecondaryTriggered: {
            root.allowDirtyClose = true
            root.close()
        }
    }
}
