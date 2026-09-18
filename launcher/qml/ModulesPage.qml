import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    property int selectedModuleIndex: -1
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
            artwork: "", capabilities: "", linkedGame: ""
        }
    }

    function selectedModule() {
        if (!hasModules || selectedModuleIndex < 0 || selectedModuleIndex >= modulesModel.count)
            return emptyModule()
        return modulesModel.get(selectedModuleIndex)
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
            return
        }
        for (var i = 0; i < modulesModel.count; ++i) {
            var module = modulesModel.get(i)
            if (matchesSearch(module.moduleName, module.moduleType, module.moduleId)) {
                selectedModuleIndex = i
                return
            }
        }
        selectedModuleIndex = -1
    }

    onSearchTextChanged: selectFirstMatchingModule()

    function genericSettings() {
        return [
            { id: "renderer", label: "Renderer override", description: "Use the launcher default or select a module-supported renderer.", type: "choice", defaultValue: "Launcher default", options: ["Launcher default", "Vulkan", "Direct3D 12"] },
            { id: "offline", label: "Offline mode", description: "Force offline-compatible services for this module.", type: "bool", defaultValue: true },
            { id: "debugOverlay", label: "Debug overlay", description: "Show module diagnostics while launching test content.", type: "bool", defaultValue: false }
        ]
    }

    function gracemeriaSettings() {
        return [
            { id: "region", label: "Game region", description: "Choose which supported executable/content region the module should prefer.", type: "choice", defaultValue: "Automatic", options: ["Automatic", "NTSC-U", "PAL"] },
            { id: "renderer", label: "Renderer override", description: "Override the launcher renderer for this module.", type: "choice", defaultValue: "Launcher default", options: ["Launcher default", "Vulkan", "Direct3D 12"] },
            { id: "offline", label: "Offline mode", description: "Use local service fallbacks while online services are unavailable.", type: "bool", defaultValue: true },
            { id: "skipIntro", label: "Skip intro videos", description: "Example module-defined preference used only by the UI preview.", type: "bool", defaultValue: false },
            { id: "cache", label: "Shader cache profile", description: "Example scalable manifest choice.", type: "choice", defaultValue: "Automatic", options: ["Automatic", "Conservative", "Aggressive"] }
        ]
    }

    // Settings remain outside ListModel delegates. Real modules will expose the
    // same schema through the module manifest/service API, and this resolver
    // can be replaced without changing ModuleSettingsDialog.
    function settingsForModule(moduleId) {
        if (moduleId === "org.nimauria.project-gracemeria")
            return gracemeriaSettings()
        if (String(moduleId).indexOf("xenon.test.") === 0)
            return genericSettings()
        return []
    }

    function populateFixtures() {
        modulesModel.clear()
        selectedModuleIndex = -1
        if (!testMode || fixtureMode === "none")
            return

        if (fixtureMode === "gracemeria") {
            modulesModel.append({
                moduleName: "Project Gracemeria",
                moduleType: "Game Module",
                version: "preview",
                status: "Active",
                active: true,
                updateAvailable: false,
                description: "Project Gracemeria launcher preview. No commercial game content is bundled; users provide their own local files.",
                moduleId: "org.nimauria.project-gracemeria",
                regions: "NTSC-U / PAL",
                gameIds: "Module declared",
                runtimeDependency: "Xenon Recomp",
                renderer: "Vulkan / Direct3D 12",
                artwork: "",
                capabilities: "Game identification • DLC catalogue • Saves • Offline services • Module settings",
                linkedGame: "Ace Combat 6: Fires of Liberation"
            })
        } else {
            modulesModel.append({
                moduleName: "Test Flight Module", moduleType: "Game Module", version: "0.1-test",
                status: "Active", active: true, updateAvailable: false,
                description: "Fictional game module used to exercise discovery, compatibility, update and module settings UI.",
                moduleId: "xenon.test.flight", regions: "Test Region A / B", gameIds: "TEST0001",
                runtimeDependency: "Xenon Recomp", renderer: "Vulkan", artwork: "",
                capabilities: "Game identification • DLC catalogue • Save data • Offline launch",
                linkedGame: "Xenon Test Flight"
            })
            modulesModel.append({
                moduleName: "Test Arena Module", moduleType: "Game Module", version: "0.2-test",
                status: "Disabled", active: false, updateAvailable: true,
                description: "Second fictional module used to verify disabled states and update indicators.",
                moduleId: "xenon.test.arena", regions: "Module-defined", gameIds: "TEST0002",
                runtimeDependency: "Xenon Recomp", renderer: "Automatic", artwork: "",
                capabilities: "Game identification • Local content validation",
                linkedGame: ""
            })
            modulesModel.append({
                moduleName: "Test Compatibility Pack", moduleType: "Support Package", version: "0.1-test",
                status: "Installed", active: true, updateAvailable: false,
                description: "Fictional shared support package used to exercise non-game module presentation.",
                moduleId: "xenon.test.compat", regions: "Global", gameIds: "Multiple",
                runtimeDependency: "Xenon Recomp", renderer: "N/A", artwork: "",
                capabilities: "Compatibility metadata • Validation definitions",
                linkedGame: ""
            })
        }
        if (modulesModel.count > 0)
            selectedModuleIndex = 0
    }

    Component.onCompleted: populateFixtures()

    Connections {
        target: launcherBridge
        function onSettingChanged(key, value) {
            if (key === "developer/fixtureMode") {
                root.fixtureMode = String(value)
                root.populateFixtures()
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        XSectionHeader {
            title: "Modules"
            description: "Modules add game-specific behaviour without coupling individual games to the Xenon runtime."
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spaceMd

            XPanel {
                Layout.preferredWidth: Math.max(310, Math.min(380, parent.width * 0.29))
                Layout.minimumWidth: 280
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.spaceMd
                    spacing: Theme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        Text { Layout.fillWidth: true; text: "Installed modules"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                        StatusPill { visible: root.testMode && root.fixtureMode !== "none"; label: root.fixtureMode === "gracemeria" ? "GRACEMERIA PREVIEW" : "TEST"; tone: Theme.warning }
                        Text { text: modulesModel.count.toString(); color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                    }

                    StackLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: root.hasModules ? 1 : 0

                        EmptyState {
                            glyph: "◇"
                            title: "No modules installed"
                            description: "Import a local Xenon module package or browse the public module catalog. Modules never contain commercial game data."
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
                                required property int index
                                required property string moduleName
                                required property string moduleType
                                required property string version
                                required property string status
                                required property bool active
                                required property bool updateAvailable
                                required property string moduleId

                                readonly property bool matches: root.matchesSearch(moduleName, moduleType, moduleId)
                                width: moduleList.width - (moduleList.ScrollBar.vertical.visible ? 8 : 0)
                                height: matches ? Math.round(94 + Math.max(0, Theme.textScale - 1.0) * 36) : 0
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
                                Keys.onReturnPressed: root.selectedModuleIndex = index
                                Keys.onEnterPressed: root.selectedModuleIndex = index
                                Accessible.onPressAction: root.selectedModuleIndex = index

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
                                            color: active ? Theme.success : Theme.accent
                                            font.pixelSize: Theme.typeBodyLarge
                                            font.weight: Font.DemiBold
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text { Layout.fillWidth: true; text: moduleName; color: Theme.text; elide: Text.ElideRight; font.pixelSize: Theme.typeBody; font.weight: Font.DemiBold }
                                            Rectangle { visible: updateAvailable; width: 8; height: 8; radius: 4; color: Theme.warning }
                                        }
                                        Text { text: moduleType + " • " + version; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                                        Text { text: updateAvailable ? "Update available" : status; color: updateAvailable ? Theme.warning : active ? Theme.success : Theme.textMuted; font.pixelSize: Theme.typeCaption }
                                    }
                                }

                                MouseArea { id: moduleMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: root.selectedModuleIndex = index }
                            }
                        }
                    }

                    XButton {
                        Layout.fillWidth: true
                        text: "+  Import Module"
                        variant: "primary"
                        onClicked: moduleDialog.open()
                    }
                    XButton {
                        visible: launcherBridge.featureEnabled("modules.catalog")
                        Layout.fillWidth: true
                        text: "Browse Module Catalog"
                        onClicked: catalogDialog.open()
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
                        description: "Select an installed module to inspect metadata, capabilities and module-defined settings."
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
                                Layout.fillWidth: true
                                implicitHeight: moduleHeaderColumn.implicitHeight + Theme.spaceXl * 2
                                color: Theme.surfaceAlt

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
                                        Text {
                                            Layout.fillWidth: true
                                            text: root.selectedModule().moduleName
                                            color: Theme.text
                                            font.pixelSize: Theme.typeTitle
                                            font.weight: Font.DemiBold
                                            wrapMode: Text.WordWrap
                                        }
                                        StatusPill {
                                            label: root.selectedModule().active ? "Active" : root.selectedModule().status
                                            tone: root.selectedModule().active ? Theme.success : Theme.textMuted
                                        }
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: root.selectedModule().moduleType + " • Version " + root.selectedModule().version
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.typeCaption
                                        wrapMode: Text.WordWrap
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
                                        Layout.topMargin: Theme.spaceXs
                                        spacing: Theme.spaceSm
                                        XButton {
                                            text: root.selectedModule().active ? "Disable" : "Enable"
                                            variant: root.selectedModule().active ? "default" : "primary"
                                            onClicked: {
                                                var nowActive = !root.selectedModule().active
                                                modulesModel.setProperty(root.selectedModuleIndex, "active", nowActive)
                                                modulesModel.setProperty(root.selectedModuleIndex, "status", nowActive ? "Active" : "Disabled")
                                            }
                                        }
                                        XButton {
                                            visible: launcherBridge.featureEnabled("modules.settings")
                                            text: "Module Settings"
                                            onClicked: settingsDialog.openFor(root.selectedModule().moduleName, root.settingsForModule(root.selectedModule().moduleId))
                                        }
                                        XButton {
                                            visible: launcherBridge.featureEnabled("modules.updates")
                                            text: root.selectedModule().updateAvailable ? "Install Update" : "Check for Updates"
                                            onClicked: launcherBridge.requestModuleUpdate(root.selectedModule().moduleId)
                                        }
                                        XButton { text: "Verify"; onClicked: launcherBridge.notifyUnavailable("Verify Module") }
                                        XButton { text: "Open Folder"; onClicked: launcherBridge.notifyUnavailable("Open Module Folder") }
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
                                    implicitHeight: Math.max(moduleInfo.implicitHeight, capabilityInfo.implicitHeight) + Theme.spaceLg * 2
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
                                    }
                                }

                                XPanel {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignTop
                                    implicitHeight: Math.max(moduleInfo.implicitHeight, capabilityInfo.implicitHeight) + Theme.spaceLg * 2
                                    ColumnLayout {
                                        id: capabilityInfo
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                        anchors.margins: Theme.spaceLg
                                        spacing: Theme.spaceSm
                                        Text { text: "Capabilities & dependencies"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                        Text { text: "Declared capabilities"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
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
                                        XInfoRow { label: "Linked game"; value: root.selectedModule().linkedGame.length > 0 ? root.selectedModule().linkedGame : "Not configured" }
                                    }
                                }
                            }

                            XPanel {
                                visible: root.testMode && root.fixtureMode !== "none"
                                Layout.fillWidth: true
                                implicitHeight: fixtureRow.implicitHeight + Theme.spaceLg * 2
                                color: Theme.accentSoft
                                GridLayout {
                                    id: fixtureRow
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                    anchors.margins: Theme.spaceLg
                                    columns: width >= 720 && Theme.textScale < 1.5 ? 2 : 1
                                    columnSpacing: Theme.spaceMd
                                    rowSpacing: Theme.spaceSm
                                    Text { Layout.fillWidth: true; text: "Fixture data only. Module controls exercise the launcher front end and do not modify real module installations."; color: Theme.textMuted; wrapMode: Text.WordWrap; font.pixelSize: Theme.typeCaption }
                                    XButton {
                                        Layout.alignment: Qt.AlignRight
                                        Layout.fillWidth: fixtureRow.columns === 1
                                        text: root.selectedModule().linkedGame.length > 0 ? "Remove “" + root.selectedModule().linkedGame + "” from Library" : "Locate Game Content"
                                        variant: root.selectedModule().linkedGame.length > 0 ? "danger" : "primary"
                                        onClicked: {
                                            if (root.selectedModule().linkedGame.length > 0)
                                                modulesModel.setProperty(root.selectedModuleIndex, "linkedGame", "")
                                            else
                                                launcherBridge.notifyUnavailable("Locate Game Content")
                                        }
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

    FileDialog {
        id: moduleDialog
        title: "Import Xenon module"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Xenon module packages (*.xenonmod *.zip)", "All files (*)"]
        onAccepted: launcherBridge.notify(
            "Module selected",
            "Selected " + selectedFiles.length + " local module package(s). Manifest validation will connect to the module registry backend later.")
    }

    ModuleSettingsDialog {
        id: settingsDialog
        onSettingEdited: function(settingId, value) {
            launcherBridge.notify("Module setting changed", settingId + " = " + String(value) + " (front-end preview)")
        }
    }

    ModuleCatalogDialog { id: catalogDialog }
}
