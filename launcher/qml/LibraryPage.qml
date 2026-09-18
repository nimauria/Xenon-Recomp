import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    property int selectedGameIndex: -1
    property bool showModuleArtwork: launcherBridge.boolSetting("appearance/artworkBackgrounds", true)
    property bool showCompatibility: launcherBridge.boolSetting("library/showCompatibility", true)
    property string fixtureMode: launcherBridge.stringSetting(
        "developer/fixtureMode", launcherBridge.testMode ? "generic" : "none")

    signal requestPage(int index)

    readonly property bool hasGames: gamesModel.count > 0
    readonly property bool testMode: launcherBridge.testMode

    ListModel { id: gamesModel }
    ListModel { id: dlcModel }

    function emptyGame() {
        return {
            title: "", moduleName: "", status: "", ready: false,
            installed: false, tileArt: "", heroArt: "", description: "",
            gameId: "", renderer: "", mode: "", regions: "", contentState: "",
            tags: "", moduleVersion: "", lastPlayed: ""
        }
    }

    function selectedGame() {
        if (!hasGames || selectedGameIndex < 0 || selectedGameIndex >= gamesModel.count)
            return emptyGame()
        return gamesModel.get(selectedGameIndex)
    }

    function matchesSearch(title, moduleName) {
        var needle = searchText.trim().toLowerCase()
        if (needle.length === 0)
            return true
        return String(title).toLowerCase().indexOf(needle) !== -1
            || String(moduleName).toLowerCase().indexOf(needle) !== -1
    }

    function selectFirstMatchingGame() {
        if (!hasGames) {
            selectedGameIndex = -1
            return
        }
        for (var i = 0; i < gamesModel.count; ++i) {
            var game = gamesModel.get(i)
            if (matchesSearch(game.title, game.moduleName)) {
                selectedGameIndex = i
                return
            }
        }
        selectedGameIndex = -1
    }

    onSearchTextChanged: selectFirstMatchingGame()

    function clearLibrary() {
        gamesModel.clear()
        dlcModel.clear()
        selectedGameIndex = -1
    }

    function moduleSettingsSchema() {
        if (fixtureMode === "gracemeria") {
            return [
                { id: "region", label: "Game region", description: "Choose the supported game region to prefer.", type: "choice", defaultValue: "Automatic", options: ["Automatic", "NTSC-U", "PAL"] },
                { id: "renderer", label: "Renderer override", description: "Override the launcher renderer for this module.", type: "choice", defaultValue: "Launcher default", options: ["Launcher default", "Vulkan", "Direct3D 12"] },
                { id: "offline", label: "Offline mode", description: "Use local service fallbacks while online services are unavailable.", type: "bool", defaultValue: true },
                { id: "debugOverlay", label: "Module diagnostics", description: "Show module-provided developer information in test builds.", type: "bool", defaultValue: false }
            ]
        }
        return [
            { id: "renderer", label: "Renderer override", description: "Example module-defined renderer preference.", type: "choice", defaultValue: "Launcher default", options: ["Launcher default", "Vulkan", "Direct3D 12"] },
            { id: "offline", label: "Offline mode", description: "Example boolean setting supplied by the module manifest.", type: "bool", defaultValue: true },
            { id: "debug", label: "Debug overlay", description: "Example test-only developer option.", type: "bool", defaultValue: false }
        ]
    }

    function populateDlcForSelection() {
        dlcModel.clear()
        if (!testMode || fixtureMode === "none" || selectedGameIndex < 0)
            return

        var items
        if (fixtureMode === "gracemeria") {
            items = [
                ["CFA-44 Nosferatu", true],
                ["Ace of Aces Mission Pack", true],
                ["F-15E Garuda Skin", false],
                ["Multiplayer Map Pack", true],
                ["Extra Music Pack", false],
                ["Special Aircraft Set", true],
                ["Additional Mission Set", false],
                ["Aircraft Skin Collection", true]
            ]
        } else if (selectedGameIndex === 0) {
            items = [
                ["Test Expansion Alpha", true], ["Test Mission Pack", true],
                ["Test Vehicle Pack", false], ["Test Cosmetic Pack", false],
                ["Test Challenge Pack", true], ["Test Aircraft Pack", true],
                ["Test Music Pack", false], ["Test Mission Beta", true],
                ["Test Livery Pack", false], ["Test Bonus Content", true]
            ]
        } else {
            items = [["Test Arena Pack", false], ["Test Ruleset Pack", true]]
        }
        for (var i = 0; i < items.length; ++i)
            dlcModel.append({ name: items[i][0], installed: items[i][1] })
    }

    function populateFixtures() {
        clearLibrary()
        if (!testMode || fixtureMode === "none")
            return

        if (fixtureMode === "gracemeria") {
            gamesModel.append({
                title: "Ace Combat 6: Fires of Liberation",
                moduleName: "Project Gracemeria",
                status: "UI Preview",
                ready: true,
                installed: true,
                tileArt: "",
                heroArt: "",
                description: "Project Gracemeria UI preview for the Xenon launcher. The module supplies metadata, compatibility rules and add-on definitions; users provide their own legally obtained game content locally.",
                gameId: "4E4D07D1",
                renderer: "Vulkan",
                mode: "Offline",
                regions: "NTSC-U / PAL",
                contentState: "Base content + title update + add-ons detected (preview)",
                tags: "Aerial Combat|Cinematic Story|Project Gracemeria",
                moduleVersion: "preview",
                lastPlayed: "Not launched"
            })
        } else {
            gamesModel.append({
                title: "Xenon Test Flight", moduleName: "Test Flight Module",
                status: "Ready to Play", ready: true, installed: true,
                tileArt: "", heroArt: "",
                description: "A fictional launcher-only entry used to exercise the Project Xenon library interface before runtime services are connected.",
                gameId: "TEST0001", renderer: "Vulkan", mode: "Offline",
                regions: "Test Region A / B", contentState: "Base content + update + add-ons detected",
                tags: "Aerial Combat|Cinematic Story|Large Battles", moduleVersion: "0.1-test",
                lastPlayed: "Not launched"
            })
            gamesModel.append({
                title: "Xenon Test Arena", moduleName: "Test Arena Module",
                status: "Content Missing", ready: false, installed: true,
                tileArt: "", heroArt: "",
                description: "A second fictional entry used to verify disabled launch states and incomplete-content indicators.",
                gameId: "TEST0002", renderer: "Automatic", mode: "Offline",
                regions: "Module Defined", contentState: "Required base content missing",
                tags: "Test Fixture|Content Validation", moduleVersion: "0.2-test",
                lastPlayed: "Not launched"
            })
        }
        selectedGameIndex = gamesModel.count > 0 ? 0 : -1
        populateDlcForSelection()
    }

    function installedDlcCount() {
        var count = 0
        for (var i = 0; i < dlcModel.count; ++i)
            if (dlcModel.get(i).installed) count += 1
        return count
    }

    function removeSelectedTestEntry() {
        if (!testMode || selectedGameIndex < 0)
            return
        gamesModel.remove(selectedGameIndex)
        selectedGameIndex = gamesModel.count > 0
            ? Math.min(selectedGameIndex, gamesModel.count - 1) : -1
        populateDlcForSelection()
    }

    onSelectedGameIndexChanged: populateDlcForSelection()

    Connections {
        target: launcherBridge
        function onSettingChanged(key, value) {
            if (key === "appearance/artworkBackgrounds")
                root.showModuleArtwork = launcherBridge.boolSetting(key, true)
            else if (key === "library/showCompatibility")
                root.showCompatibility = launcherBridge.boolSetting(key, true)
            else if (key === "developer/fixtureMode") {
                root.fixtureMode = String(value)
                root.populateFixtures()
            }
        }
    }

    Component.onCompleted: populateFixtures()

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        XPanel {
            Layout.preferredWidth: Math.max(280, Math.min(320, parent.width * 0.24))
            Layout.minimumWidth: 268
            Layout.fillHeight: true

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceSm

                RowLayout {
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: "Library"; color: Theme.text; font.pixelSize: Theme.typeSubtitle; font.weight: Font.DemiBold }
                    StatusPill { visible: root.testMode && root.fixtureMode !== "none"; label: root.fixtureMode === "gracemeria" ? "GRACEMERIA" : "TEST"; tone: Theme.warning }
                    Text { text: gamesModel.count.toString(); color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.hasGames ? 1 : 0

                    EmptyState {
                        glyph: "▣"
                        title: root.testMode && root.fixtureMode !== "none" ? "No fixture games loaded" : "Your library is empty"
                        description: "Add your own local game content. Xenon asks installed modules to identify and validate supported files; commercial game data is never distributed by Xenon."
                        primaryText: root.testMode && root.fixtureMode !== "none" ? "Reload Fixture" : "Add Game"
                        secondaryText: ""
                        onPrimaryClicked: root.testMode && root.fixtureMode !== "none" ? root.populateFixtures() : gameContentDialog.open()
                    }

                    ListView {
                        reuseItems: true
                        id: gameList
                        clip: true
                        model: gamesModel
                        spacing: Theme.spaceSm
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        delegate: Item {
                            id: gameDelegate
                            required property int index
                            required property string title
                            required property string moduleName
                            required property string status
                            required property bool ready
                            required property bool installed
                            required property string tileArt
                            readonly property bool matches: root.matchesSearch(title, moduleName)
                            width: gameList.width - (gameList.ScrollBar.vertical.visible ? 8 : 0)
                            height: matches ? gameTile.implicitHeight : 0
                            visible: matches

                            GameTile {
                                id: gameTile
                                anchors.fill: parent
                                title: gameDelegate.title
                                moduleName: gameDelegate.moduleName
                                status: gameDelegate.status
                                ready: gameDelegate.ready
                                installed: gameDelegate.installed
                                artworkSource: root.showModuleArtwork ? gameDelegate.tileArt : ""
                                selected: root.selectedGameIndex === gameDelegate.index
                                onActivated: root.selectedGameIndex = gameDelegate.index
                            }
                        }
                    }
                }

                XButton {
                    Layout.fillWidth: true
                    text: "+  Add Game"
                    variant: "primary"
                    onClicked: gameContentDialog.open()
                }
            }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.hasGames && root.selectedGameIndex >= 0 ? 1 : 0

            EmptyState {
                glyph: "X"
                title: root.searchText.trim().length > 0 && root.selectedGameIndex < 0 ? "No matching games" : "Select a game"
                description: root.searchText.trim().length > 0 && root.selectedGameIndex < 0
                    ? "Try a different library search."
                    : "Game details appear here after a compatible installed module recognises your local content."
                primaryText: ""
                secondaryText: ""
            }

            ScrollView {
                id: detailScroll
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AsNeeded

                readonly property bool wide: availableWidth >= 1120 && Theme.textScale <= 1.30

                ColumnLayout {
                    // Keep a real right gutter inside the ScrollView. Without it,
                    // the DLC panel and its status text can sit beneath the native
                    // scrollbar / application edge on Windows.
                    width: Math.max(0, detailScroll.availableWidth - Theme.spaceLg)
                    spacing: Theme.spaceMd

                    ArtworkFrame {
                        Layout.fillWidth: true
                        Layout.preferredHeight: detailScroll.wide ? 220 : 180
                        source: root.showModuleArtwork ? root.selectedGame().heroArt : ""
                        fallbackTitle: ""
                        hero: true
                        StatusPill {
                            visible: root.testMode && root.fixtureMode !== "none"
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.margins: Theme.spaceMd
                            label: root.fixtureMode === "gracemeria" ? "PROJECT GRACEMERIA UI PREVIEW" : "FICTIONAL TEST DATA"
                            tone: Theme.warning
                        }
                    }

                    GridLayout {
                        id: upperGrid
                        Layout.fillWidth: true
                        columns: detailScroll.wide ? 2 : 1
                        columnSpacing: Theme.spaceMd
                        rowSpacing: Theme.spaceMd

                        XPanel {
                            Layout.fillWidth: true
                            Layout.preferredWidth: detailScroll.wide ? Math.max(560, upperGrid.width - 384 - Theme.spaceMd) : -1
                            implicitHeight: gameSummary.implicitHeight + Theme.spaceLg * 2
                            Layout.alignment: Qt.AlignTop
                            decorated: true

                            ColumnLayout {
                                id: gameSummary
                                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                anchors.margins: Theme.spaceLg
                                spacing: Theme.spaceSm

                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.selectedGame().title
                                        color: Theme.text
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: Theme.typeTitle
                                        font.weight: Font.DemiBold
                                    }
                                    XIconButton {
                                        id: gameActionsButton
                                        iconName: "more"
                                        tooltip: "More game actions"
                                        variant: "filled"
                                        onClicked: gameActionsMenu.open()
                                        XActionMenu {
                                            id: gameActionsMenu
                                            x: gameActionsButton.width - width
                                            y: gameActionsButton.height + 4
                                            menuWidth: 290
                                            actions: [
                                                { id: "properties", label: "Game properties", icon: "ⓘ" },
                                                { id: "moduleSettings", label: "Module settings", icon: "◇" },
                                                { id: "remove", label: "Remove “" + root.selectedGame().title + "” from Library", icon: "×", destructive: true, separatorBefore: true }
                                            ]
                                            onActionTriggered: function(actionId) {
                                                if (actionId === "remove") {
                                                    if (root.testMode) root.removeSelectedTestEntry()
                                                    else launcherBridge.notifyUnavailable("Remove " + root.selectedGame().title + " from Library")
                                                } else if (actionId === "properties") {
                                                    launcherBridge.notifyUnavailable("Game Properties")
                                                } else if (actionId === "moduleSettings") {
                                                    moduleSettingsDialog.openFor(root.selectedGame().moduleName, root.moduleSettingsSchema())
                                                }
                                            }
                                        }
                                    }
                                }

                                Flow {
                                    Layout.fillWidth: true
                                    spacing: Theme.spaceSm
                                    StatusPill { label: root.selectedGame().moduleName; tone: Theme.accent }
                                    Repeater {
                                        model: root.selectedGame().tags.length > 0 ? root.selectedGame().tags.split("|") : []
                                        delegate: StatusPill { required property var modelData; label: String(modelData); tone: Theme.textMuted }
                                    }
                                    StatusPill { label: root.selectedGame().ready ? "Ready" : root.selectedGame().status; tone: root.selectedGame().ready ? Theme.success : Theme.warning }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: root.selectedGame().description
                                    color: Theme.textMuted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: Theme.typeBody
                                    lineHeight: 1.25
                                }

                                Flow {
                                    Layout.fillWidth: true
                                    spacing: Theme.spaceSm
                                    XButton {
                                        text: "▶  Play"
                                        variant: "primary"
                                        enabled: root.selectedGame().ready
                                        onClicked: launcherBridge.notifyUnavailable("Launch " + root.selectedGame().title)
                                    }
                                    XButton {
                                        id: manageFilesButton
                                        visible: launcherBridge.featureEnabled("library.manageFiles")
                                        text: "Manage Files  ▾"
                                        onClicked: manageMenu.open()
                                        XActionMenu {
                                            id: manageMenu
                                            x: 0; y: manageFilesButton.height + 5; menuWidth: 270
                                            actions: [
                                                { id: "browse", label: "Browse game files", icon: "▣" },
                                                { id: "saves", label: "Open save data", icon: "▤" },
                                                { id: "module", label: "Open module folder", icon: "◇" },
                                                { id: "verify", label: "Verify imported content", icon: "✓", separatorBefore: true }
                                            ]
                                            onActionTriggered: function(actionId) { launcherBridge.notifyUnavailable(actionId) }
                                        }
                                    }
                                }
                            }
                        }

                        XPanel {
                            visible: launcherBridge.featureEnabled("library.dlc")
                            Layout.fillWidth: true
                            Layout.preferredWidth: detailScroll.wide ? 384 : -1
                            Layout.preferredHeight: 292
                            Layout.alignment: Qt.AlignTop

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: Theme.spaceLg
                                spacing: Theme.spaceSm

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.spaceSm

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spaceSm
                                        Text { Layout.fillWidth: true; text: "DLC & Add-ons"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold; wrapMode: Text.WordWrap }
                                        StatusPill { visible: dlcModel.count > 0; label: root.installedDlcCount() + " / " + dlcModel.count; tone: Theme.textMuted }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spaceSm
                                        Text { Layout.fillWidth: true; text: "Catalogue supplied by the selected game module"; color: Theme.textMuted; font.pixelSize: Theme.typeCaption; wrapMode: Text.WordWrap }
                                        XButton { text: "+  Import DLC"; onClicked: importDlcDialog.open() }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                                Item {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    clip: true

                                    ListView {
                                        reuseItems: true
                                        id: dlcList
                                        anchors.fill: parent
                                        anchors.rightMargin: ScrollBar.vertical.visible ? Theme.spaceMd : Theme.spaceXs
                                        topMargin: Theme.spaceXs
                                        bottomMargin: Theme.spaceXs
                                        clip: true
                                        model: dlcModel
                                        spacing: 2
                                        boundsBehavior: Flickable.StopAtBounds
                                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded; interactive: true }

                                        delegate: Item {
                                            required property string name
                                            required property bool installed
                                            width: dlcList.width
                                            height: Math.max(38, Theme.controlHeight)

                                            RowLayout {
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                anchors.verticalCenter: parent.verticalCenter
                                                anchors.leftMargin: Theme.spaceXs
                                                anchors.rightMargin: Theme.spaceMd
                                                spacing: Theme.spaceSm
                                                Rectangle {
                                                    width: 20; height: 20; radius: 10; color: "transparent"
                                                    border.width: Theme.borderWidth
                                                    border.color: installed ? Theme.success : Theme.danger
                                                    Text { anchors.centerIn: parent; text: installed ? "✓" : "×"; color: installed ? Theme.success : Theme.danger; font.pixelSize: Theme.typeCaption; font.weight: Font.DemiBold }
                                                }
                                                Text { Layout.fillWidth: true; text: name; color: Theme.text; font.pixelSize: Theme.typeCaption; elide: Text.ElideRight }
                                                Text {
                                                    Layout.maximumWidth: Math.max(92, parent.width * 0.34)
                                                    text: installed ? "Installed" : "Not installed"
                                                    color: installed ? Theme.success : Theme.textMuted
                                                    font.pixelSize: Theme.typeCaption
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }
                                    }

                                    WheelHandler {
                                        target: null
                                        blocking: true
                                        orientation: Qt.Vertical
                                        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                                        onWheel: function(event) {
                                            if (dlcList.contentHeight <= dlcList.height)
                                                return
                                            var amount = event.pixelDelta.y !== 0 ? -event.pixelDelta.y : -event.angleDelta.y / 2
                                            var maximum = Math.max(0, dlcList.contentHeight - dlcList.height)
                                            dlcList.contentY = Math.max(0, Math.min(maximum, dlcList.contentY + amount))
                                        }
                                    }

                                    Text {
                                        visible: dlcModel.count === 0
                                        anchors.centerIn: parent
                                        text: "No add-on catalogue supplied by this module."
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.typeCaption
                                    }
                                }
                            }
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: detailScroll.availableWidth >= 900 && Theme.textScale <= 1.4 && root.showCompatibility ? 2 : 1
                        columnSpacing: Theme.spaceMd
                        rowSpacing: Theme.spaceMd

                        XPanel {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            implicitHeight: Math.max(gameInfo.implicitHeight, root.showCompatibility ? compatibilityInfo.implicitHeight : 0) + Theme.spaceLg * 2
                            ColumnLayout {
                                id: gameInfo
                                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                anchors.margins: Theme.spaceLg
                                spacing: Theme.spaceSm
                                Text { text: "Game information"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                XInfoRow { label: "Game ID"; value: root.selectedGame().gameId }
                                XInfoRow { label: "Module"; value: root.selectedGame().moduleName }
                                XInfoRow { label: "Module version"; value: root.selectedGame().moduleVersion || "Module defined" }
                                XInfoRow { label: "Runtime"; value: "Xenon Recomp" }
                                XInfoRow { label: "Renderer"; value: root.selectedGame().renderer }
                                XInfoRow { label: "Mode"; value: root.selectedGame().mode }
                                XInfoRow { label: "Regions"; value: root.selectedGame().regions }
                                XInfoRow { label: "Content state"; value: root.selectedGame().contentState }
                                XInfoRow { label: "Last played"; value: root.selectedGame().lastPlayed || "Not recorded" }
                                XInfoRow { label: "Status"; value: root.selectedGame().ready ? "Early development" : root.selectedGame().status }
                            }
                        }

                        XPanel {
                            visible: root.showCompatibility
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            implicitHeight: Math.max(gameInfo.implicitHeight, compatibilityInfo.implicitHeight) + Theme.spaceLg * 2
                            ColumnLayout {
                                id: compatibilityInfo
                                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                anchors.margins: Theme.spaceLg
                                spacing: Theme.spaceSm
                                Text { text: "Compatibility & module status"; color: Theme.text; font.pixelSize: Theme.typeBodyLarge; font.weight: Font.DemiBold }
                                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                                XInfoRow { label: "Module"; value: root.selectedGame().installed ? "Active" : "Not installed" }
                                XInfoRow { label: "Runtime"; value: launcherBridge.backendConnected ? "Connected" : "Front-end ready" }
                                XInfoRow { label: "Renderer"; value: root.selectedGame().renderer.length > 0 ? "Configured" : "Unavailable" }
                                XInfoRow { label: "Game launch"; value: root.selectedGame().ready ? "UI ready / backend pending" : "Unavailable" }
                                XInfoRow { label: "Audio"; value: "Backend pending" }
                                XInfoRow { label: "Online features"; value: "Not implemented" }
                                XInfoRow { label: "Achievements"; value: "Not implemented" }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.testMode ? "Fixture data is active. Commands exercise the front end only." : "Runtime-backed status updates when Xenon services become available."
                                    color: Theme.textMuted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: Theme.typeCaption
                                }
                            }
                        }
                    }

                    Item { Layout.preferredHeight: Theme.spaceSm }
                }
            }
        }
    }

    FileDialog {
        id: importDlcDialog
        title: "Import DLC from local files"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["All files (*)"]
        onAccepted: launcherBridge.notify("DLC selected", "Selected " + selectedFiles.length + " local item(s). Validation is delegated to the selected module when the backend is connected.")
    }

    FileDialog {
        id: gameContentDialog
        title: "Add game from local content"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["All files (*)"]
        onAccepted: launcherBridge.notify("Game content selected", "Selected " + selectedFiles.length + " local item(s). Xenon will ask installed modules to identify and validate supported content.")
    }

    ModuleSettingsDialog {
        id: moduleSettingsDialog
        onSettingEdited: function(settingId, value) {
            launcherBridge.notify("Module setting changed", settingId + " = " + String(value) + " (front-end preview)")
        }
    }
}
