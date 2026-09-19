import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    property int selectedModuleIndex: -1
    property string selectedModuleId: ""
    property string contextModuleId: ""
    property string pendingRemoveModuleId: ""
    property string pendingRemoveModuleName: ""
    property string pendingDisableModuleId: ""
    property string pendingDisableModuleName: ""
    property int pendingDisableLinkedGames: 0
    property string pendingRollbackModuleId: ""
    property string pendingRollbackModuleName: ""
    property string pendingRollbackVersion: ""
    property var selectedUpdateHistory: []
    property string fixtureMode: launcherBridge.stringSetting(
        "developer/fixtureMode", launcherBridge.testMode ? "generic" : "none")

    readonly property bool testMode: launcherBridge.testMode
    readonly property bool hasModules: modulesModel.count > 0

    ListModel { id: modulesModel }

    function emptyModule() {
        return {
            moduleName: "", moduleType: "", version: "", status: "",
            active: false, updateAvailable: false, description: "", moduleId: "",
            regions: "", gameIds: "", runtimeDependency: "", renderer: "",
            artwork: "", capabilities: "", linkedGame: "", linkedGameCount: 0,
            linkedGames: "", settingsCount: 0, dlcDefinitionCount: 0,
            repositoryUrl: "", publisher: "", license: "", catalogKnown: false,
            catalogVerified: false, updateStatus: "idle", updateMessage: "",
            availableVersion: "", impactMessage: "", downloadProgress: 0.0,
            canDownloadUpdate: false, canInstallUpdate: false, path: "",
            releaseUrl: "", releaseName: "", releaseNotes: "", publishedAt: "",
            assetName: "", assetSize: 0, registryEntryUrl: "",
            downloadedBytes: 0, downloadTotalBytes: 0, stagedAt: "", verifiedDigest: "",
            rollbackAvailable: false, rollbackVersion: "", rollbackCreatedAt: ""
        }
    }

    function selectedModule() {
        if (!hasModules || selectedModuleIndex < 0 || selectedModuleIndex >= modulesModel.count)
            return emptyModule()
        return modulesModel.get(selectedModuleIndex)
    }

    function reloadUpdateHistory() {
        if (selectedModuleId.length === 0) {
            selectedUpdateHistory = []
            return
        }
        selectedUpdateHistory = launcherBridge.moduleUpdateHistory(selectedModuleId)
    }

    function formatBytes(value) {
        var bytes = Number(value || 0)
        if (bytes <= 0) return "0 B"
        if (bytes < 1024) return Math.round(bytes) + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KiB"
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MiB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GiB"
    }

    function historyVersion(entry) {
        var fromVersion = String(entry.fromVersion || "")
        var toVersion = String(entry.toVersion || "")
        if (fromVersion.length > 0 && toVersion.length > 0 && fromVersion !== toVersion)
            return fromVersion + " → " + toVersion
        return toVersion.length > 0 ? toVersion : (fromVersion.length > 0 ? fromVersion : "")
    }

    function indexForModuleId(moduleId) {
        for (var i = 0; i < modulesModel.count; ++i) {
            if (String(modulesModel.get(i).moduleId) === String(moduleId))
                return i
        }
        return -1
    }

    function matchesSearch(name, type, id) {
        var needle = searchText.trim().toLowerCase()
        if (needle.length === 0)
            return true
        return String(name).toLowerCase().indexOf(needle) !== -1
            || String(type).toLowerCase().indexOf(needle) !== -1
            || String(id).toLowerCase().indexOf(needle) !== -1
    }

    function selectFirstMatchingModule() {
        if (!hasModules) {
            selectedModuleIndex = -1
            selectedModuleId = ""
            return
        }
        for (var i = 0; i < modulesModel.count; ++i) {
            var module = modulesModel.get(i)
            if (matchesSearch(module.moduleName, module.moduleType, module.moduleId)) {
                selectedModuleIndex = i
                selectedModuleId = String(module.moduleId)
                return
            }
        }
        selectedModuleIndex = -1
        selectedModuleId = ""
    }

    onSearchTextChanged: selectFirstMatchingModule()
    onSelectedModuleIdChanged: reloadUpdateHistory()

    function updateActionLabel(module) {
        var status = String(module.updateStatus || "idle")
        if (status === "checking") return "Checking…"
        if (status === "downloading") return "Downloading…"
        if (status === "installing") return "Installing…"
        if (status === "rolling-back") return "Rolling Back…"
        if (module.canInstallUpdate) return "Install Update"
        if (module.canDownloadUpdate) return "Download Update"
        return "Check for Updates"
    }

    function settingsForModule(moduleId) {
        return launcherBridge.moduleSettingsSchema(moduleId)
    }

    function populateBackendModules() {
        var preserveId = selectedModuleId.length > 0 ? selectedModuleId
                                                     : (selectedModuleIndex >= 0 && selectedModuleIndex < modulesModel.count
                                                        ? String(modulesModel.get(selectedModuleIndex).moduleId) : "")
        modulesModel.clear()
        var items = launcherBridge.moduleEntries()
        for (var i = 0; i < items.length; ++i)
            modulesModel.append(items[i])

        var preserved = preserveId.length > 0 ? indexForModuleId(preserveId) : -1
        if (preserved >= 0) {
            selectedModuleIndex = preserved
            selectedModuleId = preserveId
        } else {
            selectFirstMatchingModule()
        }
    }

    function selectIndex(index) {
        if (index < 0 || index >= modulesModel.count)
            return
        selectedModuleIndex = index
        selectedModuleId = String(modulesModel.get(index).moduleId)
    }

    function selectModuleById(moduleId) {
        var target = String(moduleId || "")
        if (target.length === 0) return false
        for (var i = 0; i < modulesModel.count; ++i) {
            if (String(modulesModel.get(i).moduleId || "") === target) {
                selectIndex(i)
                return true
            }
        }
        return false
    }

    function openCatalog() {
        catalogDialog.open()
    }

    function openSettingsFor(moduleId) {
        var index = indexForModuleId(moduleId)
        if (index >= 0)
            selectIndex(index)
        var module = selectedModule()
        settingsDialog.openFor(module.moduleName, settingsForModule(moduleId))
    }

    function requestModuleToggle(moduleId) {
        var index = indexForModuleId(moduleId)
        if (index < 0)
            return
        selectIndex(index)
        var module = selectedModule()
        if (Boolean(module.active) && Number(module.linkedGameCount || 0) > 0) {
            pendingDisableModuleId = moduleId
            pendingDisableModuleName = String(module.moduleName || moduleId)
            pendingDisableLinkedGames = Number(module.linkedGameCount || 0)
            disableModuleConfirm.open()
            return
        }
        launcherBridge.setModuleEnabled(moduleId, !Boolean(module.active))
    }

    function performModuleAction(actionId, moduleId) {
        var index = indexForModuleId(moduleId)
        if (index >= 0)
            selectIndex(index)
        var module = selectedModule()

        if (actionId === "toggleEnabled")
            requestModuleToggle(moduleId)
        else if (actionId === "settings")
            openSettingsFor(moduleId)
        else if (actionId === "checkUpdate")
            launcherBridge.requestModuleUpdateCheck(moduleId)
        else if (actionId === "downloadUpdate")
            launcherBridge.requestModuleUpdateDownload(moduleId)
        else if (actionId === "installUpdate")
            launcherBridge.requestModuleUpdateInstall(moduleId)
        else if (actionId === "rollbackUpdate") {
            pendingRollbackModuleId = moduleId
            pendingRollbackModuleName = String(module.moduleName || moduleId)
            pendingRollbackVersion = String(module.rollbackVersion || "previous version")
            rollbackModuleConfirm.open()
        } else if (actionId === "verify")
            launcherBridge.verifyModule(moduleId)
        else if (actionId === "openFolder")
            launcherBridge.openFolder(launcherBridge.modulePath(moduleId))
        else if (actionId === "openRepository")
            launcherBridge.openExternalUrl(String(module.repositoryUrl || ""))
        else if (actionId === "copyId")
            launcherBridge.copyText(moduleId)
        else if (actionId === "remove") {
            pendingRemoveModuleId = moduleId
            pendingRemoveModuleName = String(module.moduleName || moduleId)
            removeModuleConfirm.open()
        }
    }

    function performPageAction(actionId) {
        if (actionId === "browseCatalog")
            catalogDialog.open()
        else if (actionId === "importLocal")
            moduleDialog.open()
        else if (actionId === "importFolder")
            moduleFolderDialog.open()
        else if (actionId === "refreshInstalled")
            launcherBridge.refreshModules()
        else if (actionId === "refreshCatalog")
            launcherBridge.requestModuleCatalogRefresh()
        else if (actionId === "checkAllUpdates")
            launcherBridge.requestAllModuleUpdateChecks()
    }

    function openContextFor(moduleId, item, x, y) {
        var index = indexForModuleId(moduleId)
        if (index >= 0)
            selectIndex(index)
        contextModuleId = moduleId
        moduleContextMenu.actions = launcherBridge.moduleActions(moduleId)
        var mapped = item.mapToItem(root, x, y)
        moduleContextMenu.x = Math.max(0, Math.min(root.width - moduleContextMenu.width, mapped.x))
        moduleContextMenu.y = Math.max(0, Math.min(root.height - moduleContextMenu.height, mapped.y))
        moduleContextMenu.open()
    }

    function activeCount() {
        var count = 0
        for (var i = 0; i < modulesModel.count; ++i)
            if (modulesModel.get(i).active) ++count
        return count
    }

    function updateCount() {
        var count = 0
        for (var i = 0; i < modulesModel.count; ++i)
            if (modulesModel.get(i).updateAvailable) ++count
        return count
    }

    Component.onCompleted: populateBackendModules()

    Connections {
        target: launcherBridge
        function onSettingChanged(key, value) {
            if (key === "developer/fixtureMode") {
                root.fixtureMode = launcherBridge.stringSetting(key, launcherBridge.testMode ? "generic" : "none")
                root.populateBackendModules()
            }
        }
        function onModulesChanged() { root.populateBackendModules() }
        function onModuleUpdateStateChanged(moduleId) { root.populateBackendModules() }
        function onModuleUpdateHistoryChanged(moduleId) {
            if (String(moduleId || "") === root.selectedModuleId || String(moduleId || "").length === 0)
                root.reloadUpdateHistory()
        }
        function onModuleCatalogChanged() { root.populateBackendModules() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        XSectionHeader {
            Layout.fillWidth: true
            title: "Modules"
            description: "Install, configure and update game-specific Xenon modules without coupling games to the runtime."
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            StatusPill { label: modulesModel.count + " installed"; tone: Theme.textMuted }
            StatusPill { label: root.activeCount() + " enabled"; tone: Theme.success }
            StatusPill {
                visible: root.updateCount() > 0
                label: root.updateCount() + " update" + (root.updateCount() === 1 ? "" : "s")
                tone: Theme.warning
            }
            StatusPill {
                visible: root.testMode && root.fixtureMode !== "none"
                label: root.fixtureMode === "gracemeria" ? "GRACEMERIA PREVIEW" : "TEST"
                tone: Theme.warning
            }
            Item { Layout.fillWidth: true }
            XButton {
                text: "Check All Updates"
                onClicked: launcherBridge.requestAllModuleUpdateChecks()
            }
            XButton {
                text: "Browse Catalog"
                variant: "primary"
                onClicked: catalogDialog.open()
            }
            XIconButton {
                id: pageActionsButton
                iconName: "more"
                tooltip: "More module actions"
                variant: "filled"
                onClicked: pageActionsMenu.visible ? pageActionsMenu.close() : pageActionsMenu.open()

                XActionMenu {
                    id: pageActionsMenu
                    parent: pageActionsButton
                    x: pageActionsButton.width - width
                    y: pageActionsButton.height + Theme.spaceXs
                    menuWidth: 280
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                    actions: launcherBridge.modulePageActions()
                    onActionTriggered: function(actionId) { root.performPageAction(actionId) }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spaceMd

            XPanel {
                Layout.preferredWidth: Math.max(320, Math.min(400, parent.width * 0.30))
                Layout.minimumWidth: 290
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spaceMd
                    spacing: Theme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: "Installed modules"
                            color: Theme.text
                            font.pixelSize: Theme.typeBodyLarge
                            font.weight: Font.DemiBold
                        }
                        Text { text: modulesModel.count.toString(); color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                    }

                    XTextField {
                        Layout.fillWidth: true
                        placeholderText: "Search modules…"
                        accessibleName: "Search installed modules"
                        text: root.searchText
                        onTextChanged: root.searchText = text
                    }

                    StackLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: root.hasModules ? 1 : 0

                        EmptyState {
                            glyph: "◇"
                            title: "No modules installed"
                            description: "Browse the official Xenon Modules registry or import a local module package."
                            primaryText: "Browse Modules"
                            secondaryText: "Import Local Module"
                            onPrimaryClicked: catalogDialog.open()
                            onSecondaryClicked: moduleDialog.open()
                        }

                        ListView {
                            reuseItems: true
                            id: moduleList
                            clip: true
                            spacing: Theme.spaceSm
                            model: modulesModel
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                            delegate: Rectangle {
                                id: moduleDelegate
                                required property int index
                                required property string moduleName
                                required property string moduleType
                                required property string version
                                required property string status
                                required property bool active
                                required property bool updateAvailable
                                required property string moduleId
                                required property string updateStatus
                                required property int linkedGameCount

                                readonly property bool matches: root.matchesSearch(moduleName, moduleType, moduleId)
                                width: moduleList.width - (moduleList.ScrollBar.vertical.visible ? 8 : 0)
                                height: matches ? Math.round(100 + Math.max(0, Theme.textScale - 1.0) * 40) : 0
                                visible: matches
                                radius: Theme.controlRadius
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: moduleName + ", " + (updateAvailable ? "update available" : status)
                                color: root.selectedModuleIndex === index ? Theme.accentSoft
                                     : moduleMouse.containsMouse ? Theme.surfaceHover : Theme.surface
                                border.width: activeFocus ? Theme.focusWidth : Theme.borderWidth
                                border.color: activeFocus ? Theme.focusRing
                                             : root.selectedModuleIndex === index ? Theme.accent : Theme.border
                                opacity: active ? 1.0 : 0.78
                                Keys.onReturnPressed: root.selectIndex(index)
                                Keys.onEnterPressed: root.selectIndex(index)
                                Accessible.onPressAction: root.selectIndex(index)

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: Theme.spaceMd
                                    spacing: Theme.spaceSm

                                    Rectangle {
                                        width: 48; height: 48; radius: Theme.controlRadius
                                        color: Theme.surfaceAlt
                                        border.width: Theme.borderWidth
                                        border.color: active ? Theme.success : Theme.border
                                        Text {
                                            anchors.centerIn: parent
                                            text: moduleType === "Game Module" ? "G" : "S"
                                            color: active ? Theme.success : Theme.textMuted
                                            font.pixelSize: Theme.typeBodyLarge
                                            font.weight: Font.DemiBold
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                Layout.fillWidth: true
                                                text: moduleName
                                                color: Theme.text
                                                elide: Text.ElideRight
                                                font.pixelSize: Theme.typeBody
                                                font.weight: Font.DemiBold
                                            }
                                            Rectangle { visible: updateAvailable; width: 8; height: 8; radius: 4; color: Theme.warning }
                                        }
                                        Text {
                                            text: moduleType + " • " + version
                                            color: Theme.textMuted
                                            font.pixelSize: Theme.typeCaption
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: !active ? "Disabled" : updateAvailable ? "Update available" : linkedGameCount > 0 ? linkedGameCount + " linked game" + (linkedGameCount === 1 ? "" : "s") : status
                                            color: !active ? Theme.warning : updateAvailable ? Theme.warning : active ? Theme.success : Theme.textMuted
                                            font.pixelSize: Theme.typeCaption
                                            elide: Text.ElideRight
                                        }
                                    }
                                }

                                MouseArea {
                                    id: moduleMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: function(mouse) {
                                        root.selectIndex(index)
                                        if (mouse.button === Qt.RightButton)
                                            root.openContextFor(moduleId, moduleDelegate, mouse.x, mouse.y)
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceSm
                        XButton {
                            Layout.fillWidth: true
                            text: "+  Import Package"
                            variant: "primary"
                            onClicked: moduleDialog.open()
                        }
                        XButton {
                            text: "Folder"
                            onClicked: moduleFolderDialog.open()
                        }
                        XIconButton {
                            glyph: "↻"
                            tooltip: "Refresh installed modules"
                            variant: "filled"
                            onClicked: launcherBridge.refreshModules()
                        }
                    }
                }
            }

            XPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true

                StackLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spaceLg
                    currentIndex: root.hasModules && root.selectedModuleIndex >= 0 ? 1 : 0

                    EmptyState {
                        glyph: "◇"
                        title: "Select a module"
                        description: "Select an installed module to inspect its metadata, linked games, capabilities, settings and update state."
                        primaryText: ""
                        secondaryText: ""
                    }

                    ScrollView {
                        id: moduleDetailScroll
                        clip: true
                        contentWidth: availableWidth
                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                        ScrollBar.vertical.policy: ScrollBar.AsNeeded

                        ColumnLayout {
                            width: moduleDetailScroll.availableWidth
                            spacing: Theme.spaceMd

                            XPanel {
                                id: selectedHeader
                                Layout.fillWidth: true
                                implicitHeight: moduleHeaderColumn.implicitHeight + Theme.spaceXl * 2
                                color: Theme.highContrast ? Theme.surfaceAlt : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)
                                decorated: true

                                ColumnLayout {
                                    id: moduleHeaderColumn
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Theme.spaceXl
                                    spacing: Theme.spaceSm

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spaceMd
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2
                                            Text {
                                                Layout.fillWidth: true
                                                text: root.selectedModule().moduleName
                                                color: Theme.text
                                                font.pixelSize: Theme.typeTitle
                                                font.weight: Font.DemiBold
                                                wrapMode: Text.WordWrap
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: root.selectedModule().moduleType + " • Version " + root.selectedModule().version
                                                color: Theme.textMuted
                                                font.pixelSize: Theme.typeCaption
                                            }
                                        }
                                        StatusPill {
                                            label: root.selectedModule().active ? "Enabled" : "Disabled"
                                            tone: root.selectedModule().active ? Theme.success : Theme.warning
                                        }
                                        StatusPill {
                                            visible: root.selectedModule().catalogVerified
                                            label: "OFFICIAL"
                                            tone: Theme.accent
                                        }
                                        XIconButton {
                                            id: moduleMoreButton
                                            iconName: "more"
                                            tooltip: "Module actions"
                                            variant: "filled"
                                            onClicked: {
                                                moduleMoreMenu.actions = launcherBridge.moduleActions(root.selectedModule().moduleId)
                                                moduleMoreMenu.open()
                                            }
                                            XActionMenu {
                                                id: moduleMoreMenu
                                                x: moduleMoreButton.width - width
                                                y: moduleMoreButton.height + Theme.spaceXs
                                                menuWidth: 285
                                                onActionTriggered: function(actionId) {
                                                    root.performModuleAction(actionId, root.selectedModule().moduleId)
                                                }
                                            }
                                        }
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: root.selectedModule().description
                                        color: Theme.textMuted
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: Theme.typeBody
                                        lineHeight: 1.25
                                    }

                                    Flow {
                                        Layout.fillWidth: true
                                        spacing: Theme.spaceSm
                                        XButton {
                                            text: root.selectedModule().active ? "Disable" : "Enable"
                                            variant: root.selectedModule().active ? "default" : "primary"
                                            onClicked: root.requestModuleToggle(root.selectedModule().moduleId)
                                        }
                                        XButton {
                                            visible: root.selectedModule().settingsCount > 0
                                            text: "Module Settings"
                                            onClicked: root.openSettingsFor(root.selectedModule().moduleId)
                                        }
                                        XButton {
                                            text: root.updateActionLabel(root.selectedModule())
                                            variant: root.selectedModule().canInstallUpdate || root.selectedModule().canDownloadUpdate ? "primary" : "default"
                                            enabled: root.selectedModule().updateStatus !== "checking"
                                                && root.selectedModule().updateStatus !== "downloading"
                                                && root.selectedModule().updateStatus !== "installing"
                                                && root.selectedModule().updateStatus !== "rolling-back"
                                            onClicked: {
                                                if (root.selectedModule().canInstallUpdate)
                                                    launcherBridge.requestModuleUpdateInstall(root.selectedModule().moduleId)
                                                else if (root.selectedModule().canDownloadUpdate)
                                                    launcherBridge.requestModuleUpdateDownload(root.selectedModule().moduleId)
                                                else
                                                    launcherBridge.requestModuleUpdateCheck(root.selectedModule().moduleId)
                                            }
                                        }
                                        XButton {
                                            visible: Boolean(root.selectedModule().rollbackAvailable)
                                            text: String(root.selectedModule().rollbackVersion || "").length > 0
                                                ? "Roll Back to " + root.selectedModule().rollbackVersion
                                                : "Roll Back"
                                            enabled: root.selectedModule().updateStatus !== "checking"
                                                && root.selectedModule().updateStatus !== "downloading"
                                                && root.selectedModule().updateStatus !== "installing"
                                                && root.selectedModule().updateStatus !== "rolling-back"
                                            onClicked: {
                                                root.pendingRollbackModuleId = root.selectedModule().moduleId
                                                root.pendingRollbackModuleName = root.selectedModule().moduleName
                                                root.pendingRollbackVersion = String(root.selectedModule().rollbackVersion || "previous version")
                                                rollbackModuleConfirm.open()
                                            }
                                        }
                                        XButton {
                                            text: "Verify"
                                            onClicked: launcherBridge.verifyModule(root.selectedModule().moduleId)
                                        }
                                        XButton {
                                            enabled: String(root.selectedModule().path || "").length > 0
                                            text: "Open Folder"
                                            onClicked: launcherBridge.openFolder(launcherBridge.modulePath(root.selectedModule().moduleId))
                                        }
                                    }
                                }

                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    gesturePolicy: TapHandler.WithinBounds
                                    onTapped: function(eventPoint, button) {
                                        root.openContextFor(root.selectedModule().moduleId, selectedHeader,
                                                            eventPoint.position.x, eventPoint.position.y)
                                    }
                                }
                            }

                            XPanel {
                                visible: !root.selectedModule().active && root.selectedModule().linkedGameCount > 0
                                Layout.fillWidth: true
                                implicitHeight: disabledImpact.implicitHeight + Theme.spaceLg * 2
                                color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.10)
                                RowLayout {
                                    id: disabledImpact
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Theme.spaceLg
                                    spacing: Theme.spaceMd
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.selectedModule().impactMessage
                                        color: Theme.text
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: Theme.typeBody
                                    }
                                    XButton {
                                        text: "Enable Module"
                                        variant: "primary"
                                        onClicked: launcherBridge.setModuleEnabled(root.selectedModule().moduleId, true)
                                    }
                                }
                            }

                            XPanel {
                                Layout.fillWidth: true
                                implicitHeight: updateColumn.implicitHeight + Theme.spaceLg * 2
                                ColumnLayout {
                                    id: updateColumn
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Theme.spaceLg
                                    spacing: Theme.spaceSm

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            Layout.fillWidth: true
                                            text: "Module updater"
                                            color: Theme.text
                                            font.pixelSize: Theme.typeBodyLarge
                                            font.weight: Font.DemiBold
                                        }
                                        StatusPill {
                                            label: root.selectedModule().updateStatus === "update-available" ? "UPDATE AVAILABLE"
                                                 : root.selectedModule().updateStatus === "ready-to-install" ? "VERIFIED / READY"
                                                 : root.selectedModule().updateStatus === "up-to-date" ? "CURRENT"
                                                 : root.selectedModule().updateStatus === "rolling-back" ? "ROLLING BACK"
                                                 : root.selectedModule().updateStatus === "rolled-back" ? "ROLLED BACK"
                                                 : root.selectedModule().updateStatus === "installing" ? "INSTALLING"
                                                 : String(root.selectedModule().updateStatus || "IDLE").toUpperCase()
                                            tone: root.selectedModule().updateAvailable ? Theme.warning
                                                : root.selectedModule().updateStatus === "up-to-date"
                                                   || root.selectedModule().updateStatus === "rolled-back" ? Theme.success
                                                : root.selectedModule().updateStatus === "error" ? Theme.danger : Theme.textMuted
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.selectedModule().updateMessage || "Use the official Xenon Modules registry entry to check this module's GitHub Releases."
                                        color: Theme.textMuted
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: Theme.typeCaption
                                    }
                                    XInfoRow {
                                        label: "Installed"
                                        value: root.selectedModule().version || "Unknown"
                                    }
                                    XInfoRow {
                                        visible: String(root.selectedModule().availableVersion || "").length > 0
                                        label: "Available"
                                        value: root.selectedModule().availableVersion
                                    }
                                    XInfoRow {
                                        visible: Boolean(root.selectedModule().rollbackAvailable)
                                        label: "Rollback retained"
                                        value: String(root.selectedModule().rollbackVersion || "Previous version")
                                    }
                                    XInfoRow {
                                        visible: root.selectedModule().updateStatus === "ready-to-install"
                                        label: "Package verification"
                                        value: "SHA-256 verified"
                                    }
                                    ProgressBar {
                                        visible: root.selectedModule().updateStatus === "downloading"
                                        Layout.fillWidth: true
                                        from: 0; to: 1
                                        value: Number(root.selectedModule().downloadProgress || 0)
                                    }
                                    Text {
                                        visible: root.selectedModule().updateStatus === "downloading"
                                        Layout.fillWidth: true
                                        text: root.formatBytes(root.selectedModule().downloadedBytes)
                                            + (Number(root.selectedModule().downloadTotalBytes || 0) > 0
                                               ? " / " + root.formatBytes(root.selectedModule().downloadTotalBytes)
                                                 + " • " + Math.round(Number(root.selectedModule().downloadProgress || 0) * 100) + "%"
                                               : "")
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.typeCaption
                                    }
                                }
                            }

                            XPanel {
                                Layout.fillWidth: true
                                implicitHeight: updateHistoryColumn.implicitHeight + Theme.spaceLg * 2

                                ColumnLayout {
                                    id: updateHistoryColumn
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Theme.spaceLg
                                    spacing: Theme.spaceSm

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            Layout.fillWidth: true
                                            text: "Update history"
                                            color: Theme.text
                                            font.pixelSize: Theme.typeBodyLarge
                                            font.weight: Font.DemiBold
                                        }
                                        XButton {
                                            visible: root.selectedUpdateHistory.length > 0
                                            text: "Clear History"
                                            variant: "ghost"
                                            onClicked: launcherBridge.clearModuleUpdateHistory(root.selectedModule().moduleId)
                                        }
                                    }
                                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                    Text {
                                        visible: root.selectedUpdateHistory.length === 0
                                        Layout.fillWidth: true
                                        text: "No persistent update events have been recorded for this module yet."
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.typeCaption
                                        wrapMode: Text.WordWrap
                                    }
                                    Repeater {
                                        model: root.selectedUpdateHistory.slice(0, 5)
                                        delegate: RowLayout {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            spacing: Theme.spaceSm
                                            StatusPill {
                                                label: String(modelData.outcome || "unknown").toUpperCase()
                                                tone: String(modelData.outcome || "") === "success" ? Theme.success
                                                    : String(modelData.outcome || "") === "failure" ? Theme.danger
                                                    : Theme.warning
                                            }
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 1
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: String(modelData.action || "update")
                                                        + (root.historyVersion(modelData).length > 0 ? " • " + root.historyVersion(modelData) : "")
                                                    color: Theme.text
                                                    font.pixelSize: Theme.typeCaption
                                                    font.weight: Font.DemiBold
                                                    elide: Text.ElideRight
                                                }
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: String(modelData.message || "")
                                                    color: Theme.textMuted
                                                    font.pixelSize: Theme.typeCaption
                                                    elide: Text.ElideRight
                                                }
                                            }
                                            Text {
                                                text: String(modelData.timestamp || "").replace("T", " ").replace("Z", "")
                                                color: Theme.textMuted
                                                font.pixelSize: Theme.typeCaption
                                            }
                                        }
                                    }
                                }
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: width >= 780 && Theme.textScale <= 1.5 ? 2 : 1
                                columnSpacing: Theme.spaceMd
                                rowSpacing: Theme.spaceMd

                                XPanel {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignTop
                                    implicitHeight: moduleInfo.implicitHeight + Theme.spaceLg * 2
                                    ColumnLayout {
                                        id: moduleInfo
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                        anchors.margins: Theme.spaceLg
                                        spacing: Theme.spaceSm
                                        Text { text: "Module information"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                        XInfoRow { label: "Module ID"; value: root.selectedModule().moduleId }
                                        XInfoRow { label: "Type"; value: root.selectedModule().moduleType }
                                        XInfoRow { label: "Version"; value: root.selectedModule().version }
                                        XInfoRow { label: "Supported regions"; value: root.selectedModule().regions }
                                        XInfoRow { label: "Supported game IDs"; value: root.selectedModule().gameIds }
                                        XInfoRow { label: "Runtime dependency"; value: root.selectedModule().runtimeDependency }
                                        XInfoRow { label: "Renderer"; value: root.selectedModule().renderer }
                                        XInfoRow { label: "Settings"; value: root.selectedModule().settingsCount + " declared" }
                                        XInfoRow { label: "DLC definitions"; value: root.selectedModule().dlcDefinitionCount + " declared" }
                                    }
                                }

                                XPanel {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignTop
                                    implicitHeight: catalogInfo.implicitHeight + Theme.spaceLg * 2
                                    ColumnLayout {
                                        id: catalogInfo
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                        anchors.margins: Theme.spaceLg
                                        spacing: Theme.spaceSm
                                        Text { text: "Catalog & source"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                        XInfoRow { label: "Xenon Modules registry"; value: root.selectedModule().catalogKnown ? "Listed" : "Not listed" }
                                        XInfoRow { label: "Publisher"; value: root.selectedModule().publisher || "Module-defined" }
                                        XInfoRow { label: "License"; value: root.selectedModule().license || "Module-defined" }
                                        XInfoRow { label: "Package trust"; value: root.selectedModule().catalogVerified ? "Verified publisher + GitHub SHA-256" : root.selectedModule().catalogKnown ? "Registry listed" : "Local / unmanaged" }
                                        XInfoRow { label: "Repository"; value: root.selectedModule().repositoryUrl || "Not declared" }
                                        XButton {
                                            visible: String(root.selectedModule().repositoryUrl || "").length > 0
                                            text: "Open Repository"
                                            onClicked: launcherBridge.openExternalUrl(root.selectedModule().repositoryUrl)
                                        }
                                        XButton {
                                            visible: String(root.selectedModule().registryEntryUrl || "").length > 0
                                            text: "View Registry Entry"
                                            onClicked: launcherBridge.openExternalUrl(root.selectedModule().registryEntryUrl)
                                        }
                                    }
                                }

                                XPanel {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignTop
                                    implicitHeight: linkedInfo.implicitHeight + Theme.spaceLg * 2
                                    ColumnLayout {
                                        id: linkedInfo
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                        anchors.margins: Theme.spaceLg
                                        spacing: Theme.spaceSm
                                        Text { text: "Library integration"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                        XInfoRow { label: "Linked games"; value: root.selectedModule().linkedGameCount.toString() }
                                        Text {
                                            Layout.fillWidth: true
                                            text: root.selectedModule().linkedGames.length > 0 ? root.selectedModule().linkedGames : "No current Library entries depend on this module."
                                            color: Theme.textMuted
                                            wrapMode: Text.WordWrap
                                            font.pixelSize: Theme.typeCaption
                                        }
                                        Text {
                                            visible: !root.selectedModule().active && root.selectedModule().linkedGameCount > 0
                                            Layout.fillWidth: true
                                            text: "Disabled modules do not hide their games. Library keeps those entries visible and marks them as Module disabled until you re-enable the module."
                                            color: Theme.warning
                                            wrapMode: Text.WordWrap
                                            font.pixelSize: Theme.typeCaption
                                        }
                                    }
                                }

                                XPanel {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignTop
                                    implicitHeight: capabilityInfo.implicitHeight + Theme.spaceLg * 2
                                    ColumnLayout {
                                        id: capabilityInfo
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                        anchors.margins: Theme.spaceLg
                                        spacing: Theme.spaceSm
                                        Text { text: "Capabilities"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                        Flow {
                                            Layout.fillWidth: true
                                            spacing: Theme.spaceXs
                                            Repeater {
                                                model: root.selectedModule().capabilities.length > 0
                                                    ? root.selectedModule().capabilities.split(" • ") : []
                                                delegate: StatusPill {
                                                    required property var modelData
                                                    label: String(modelData)
                                                    tone: Theme.textMuted
                                                }
                                            }
                                        }
                                        XInfoRow { label: "Xenon runtime"; value: launcherBridge.backendConnected ? "Connected" : "Front-end ready" }
                                        XInfoRow { label: "Manifest"; value: "Loaded" }
                                    }
                                }
                            }

                            Item { Layout.preferredHeight: Theme.spaceSm }
                        }
                    }
                }
            }
        }
    }

    XActionMenu {
        id: moduleContextMenu
        menuWidth: 285
        onActionTriggered: function(actionId) { root.performModuleAction(actionId, root.contextModuleId) }
    }

    XConfirmDialog {
        id: disableModuleConfirm
        title: "Disable “" + root.pendingDisableModuleName + "”?"
        message: (root.pendingDisableLinkedGames === 1
                  ? "1 Library game depends on this module."
                  : root.pendingDisableLinkedGames + " Library games depend on this module.")
                 + " The game" + (root.pendingDisableLinkedGames === 1 ? " will" : "s will")
                 + " remain visible, but cannot be launched until the module is enabled again."
        confirmText: "Disable Module"
        destructive: true
        onConfirmed: {
            launcherBridge.setModuleEnabled(root.pendingDisableModuleId, false)
            root.pendingDisableModuleId = ""
            root.pendingDisableModuleName = ""
            root.pendingDisableLinkedGames = 0
        }
    }

    XConfirmDialog {
        id: rollbackModuleConfirm
        title: "Roll back “" + root.pendingRollbackModuleName + "”?"
        message: "Restore the retained module snapshot for version " + root.pendingRollbackVersion
               + "? Xenon will keep the currently installed version as a rollback snapshot where possible."
        confirmText: "Roll Back"
        onConfirmed: {
            launcherBridge.requestModuleRollback(root.pendingRollbackModuleId)
            root.pendingRollbackModuleId = ""
            root.pendingRollbackModuleName = ""
            root.pendingRollbackVersion = ""
        }
    }

    XConfirmDialog {
        id: removeModuleConfirm
        title: "Remove module?"
        message: "Removing “" + root.pendingRemoveModuleName + "” deletes only its launcher-managed module directory. Games that depend on it stay in Library and will be marked Module missing until the module is installed again."
        confirmText: "Remove Module"
        destructive: true
        onConfirmed: {
            if (launcherBridge.removeModule(root.pendingRemoveModuleId))
                root.populateBackendModules()
            root.pendingRemoveModuleId = ""
            root.pendingRemoveModuleName = ""
        }
    }

    FileDialog {
        id: moduleDialog
        title: "Import Xenon module package"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Xenon module packages (*.xenonmod.zip *.zip)", "ZIP packages (*.zip)"]
        onAccepted: {
            if (launcherBridge.importModulePackages(selectedFiles))
                root.populateBackendModules()
        }
    }

    FolderDialog {
        id: moduleFolderDialog
        title: "Import unpacked Xenon module folder"
        onAccepted: {
            if (launcherBridge.importModulePackages([selectedFolder]))
                root.populateBackendModules()
        }
    }

    ModuleSettingsDialog {
        id: settingsDialog
        onSettingEdited: function(settingId, value) {
            launcherBridge.setModuleSetting(root.selectedModule().moduleId, settingId, value)
        }
    }

    ModuleCatalogDialog { id: catalogDialog }
}
