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
    property string pendingDlcGameId: ""
    property string pendingDlcId: ""
    property string pendingDlcName: ""
    property string pendingImportDlcId: ""
    property string pendingRemoveGameId: ""
    property int contextGameIndex: -1
    property string contextDlcGameId: ""
    property string contextDlcId: ""
    property string contextDlcName: ""

    signal requestPage(int index)

    readonly property bool hasGames: gamesModel.count > 0
    readonly property bool testMode: launcherBridge.testMode
    readonly property var currentSession: launcherBridge.currentSession

    function sessionMatchesSelected() {
        return root.selectedGame().gameId.length > 0
            && String(root.currentSession.gameId || "") === root.selectedGame().gameId
    }

    function selectedSessionState() {
        return root.sessionMatchesSelected() ? String(root.currentSession.state || "idle") : "idle"
    }

    function sessionActiveForOtherGame() {
        return Boolean(root.currentSession.active)
            && String(root.currentSession.gameId || "") !== root.selectedGame().gameId
    }

    function launchButtonText() {
        if (root.sessionActiveForOtherGame()) return "Session active"
        switch (root.selectedSessionState()) {
        case "preparing": return "×  Cancel Preparing"
        case "validating": return "×  Cancel Validation"
        case "starting": return "×  Cancel Starting"
        case "running": return "■  Stop Game"
        case "stopping": return "Stopping…"
        case "failed": return "↻  Try Again"
        default: return "▶  Play"
        }
    }

    function launchButtonEnabled() {
        if (!root.selectedGame().ready) return false
        if (root.sessionActiveForOtherGame()) return false
        return root.selectedSessionState() !== "stopping"
    }

    function launchButtonVariant() {
        var state = root.selectedSessionState()
        return state === "running" || state === "preparing" || state === "validating" || state === "starting"
            ? "danger" : "primary"
    }

    function activateSelectedGameSession() {
        var state = root.selectedSessionState()
        if (state === "preparing" || state === "validating" || state === "starting" || state === "running")
            launcherBridge.stopGame()
        else
            launcherBridge.launchGame(root.selectedGame().gameId)
    }

    function formatDuration(milliseconds) {
        var seconds = Math.max(0, Math.floor(Number(milliseconds || 0) / 1000))
        var hours = Math.floor(seconds / 3600)
        var minutes = Math.floor((seconds % 3600) / 60)
        if (hours > 0) return hours + "h " + minutes + "m"
        if (minutes > 0) return minutes + "m"
        return seconds + "s"
    }

    ListModel { id: gamesModel }
    ListModel { id: dlcModel }

    function emptyGame() {
        return {
            title: "", moduleName: "", status: "", ready: false,
            installed: false, tileArt: "", heroArt: "", description: "",
            gameId: "", titleId: "", renderer: "", mode: "", regions: "", contentState: "",
            tags: "", moduleVersion: "", lastPlayed: "",
            gameDeveloper: "", gamePublisher: "", gamePlatform: "", releaseYear: 0,
            compatibilityStatus: "", compatibilityLabel: "", compatibilitySummary: "",
            metadataAvailable: false, metadataSource: "", metadataStatus: "",
            metadataStatusMessage: "", metadataLastCheckedAt: ""
        }
    }

    function selectedGame() {
        if (!hasGames || selectedGameIndex < 0 || selectedGameIndex >= gamesModel.count)
            return emptyGame()
        return gamesModel.get(selectedGameIndex)
    }

    function selectGameById(gameId) {
        var target = String(gameId || "")
        if (target.length === 0) return false
        for (var i = 0; i < gamesModel.count; ++i) {
            if (String(gamesModel.get(i).gameId || "") === target) {
                selectedGameIndex = i
                populateDlcForSelection()
                return true
            }
        }
        return false
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

    function populateBackendLibrary() {
        clearLibrary()
        var items = launcherBridge.libraryEntries()
        for (var i = 0; i < items.length; ++i)
            gamesModel.append(items[i])
        selectedGameIndex = gamesModel.count > 0 ? 0 : -1
        populateDlcForSelection()
    }

    function moduleSettingsSchema() {
        return launcherBridge.moduleSettingsSchema(root.selectedGame().moduleId || "")
    }

    function populateDlcForSelection() {
        dlcModel.clear()
        if (selectedGameIndex < 0)
            return
        var items = launcherBridge.libraryDlcEntries(root.selectedGame().gameId || "")
        for (var i = 0; i < items.length; ++i)
            dlcModel.append(items[i])
    }


    function openGameProperties(gameId) {
        gamePropertiesLoader.active = true
        Qt.callLater(function() {
            if (gamePropertiesLoader.item)
                gamePropertiesLoader.item.openFor(gameId)
        })
    }

    function runGameAction(actionId, index) {
        if (index < 0 || index >= gamesModel.count)
            return
        root.selectedGameIndex = index
        var game = gamesModel.get(index)
        if (actionId === "enableModule") {
            launcherBridge.setModuleEnabled(game.moduleId || "", true)
        } else if (actionId === "play") {
            launcherBridge.launchGame(game.gameId)
        } else if (actionId === "properties") {
            root.openGameProperties(game.gameId)
        } else if (actionId === "refreshMetadata") {
            launcherBridge.refreshLibraryMetadata(game.gameId)
        } else if (actionId === "moduleSettings") {
            moduleSettingsDialog.openFor(game.moduleName, launcherBridge.moduleSettingsSchema(game.moduleId || ""))
        } else if (actionId === "browse") {
            launcherBridge.openFolder(launcherBridge.libraryContentFolder(game.gameId))
        } else if (actionId === "saves") {
            var props = launcherBridge.libraryGameProperties(game.gameId)
            launcherBridge.openFolder(String(props.savePath || ""))
        } else if (actionId === "module") {
            launcherBridge.openFolder(launcherBridge.modulePath(game.moduleId || ""))
        } else if (actionId === "verify") {
            launcherBridge.verifyLibraryEntry(game.gameId)
        } else if (actionId === "copyId") {
            launcherBridge.copyText(String(game.gameId || ""))
            launcherBridge.notify("Game ID copied", String(game.gameId || ""))
        } else if (actionId === "remove") {
            root.pendingRemoveGameId = game.gameId
            removeGameConfirm.title = "Remove “" + game.title + "” from Library?"
            removeGameConfirm.message = "This removes only the launcher library record. Registered game files and Xenon-managed DLC are not deleted."
            removeGameConfirm.open()
        }
    }

    function openGameContext(index, item, localX, localY) {
        if (index < 0 || index >= gamesModel.count)
            return
        root.selectedGameIndex = index
        root.contextGameIndex = index
        var game = gamesModel.get(index)
        libraryContextMenu.actions = launcherBridge.libraryGameActions(game.gameId)
        libraryContextMenu.openAt(item, localX, localY)
    }

    function openLibraryBackgroundContext(item, localX, localY) {
        root.contextGameIndex = -1
        libraryBackgroundMenu.actions = launcherBridge.libraryBackgroundActions()
        libraryBackgroundMenu.openAt(item, localX, localY)
    }

    function runLibraryBackgroundAction(actionId) {
        if (actionId === "add")
            gameContentDialog.open()
    }

    function runDlcAction(actionId, gameId, dlcId, name) {
        if (actionId === "open") {
            launcherBridge.openFolder(launcherBridge.libraryDlcItemFolder(gameId, dlcId))
        } else if (actionId === "verify") {
            launcherBridge.verifyLibraryDlc(gameId, dlcId)
        } else if (actionId === "remove") {
            root.pendingDlcGameId = gameId
            root.pendingDlcId = dlcId
            root.pendingDlcName = name
            removeDlcConfirm.open()
        } else if (actionId === "import") {
            root.pendingImportDlcId = dlcId
            importDlcDialog.open()
        }
    }

    function openDlcContext(index, item, localX, localY) {
        if (index < 0 || index >= dlcModel.count)
            return
        var dlc = dlcModel.get(index)
        var gameId = root.selectedGame().gameId || ""
        root.contextDlcGameId = gameId
        root.contextDlcId = String(dlc.dlcId || "")
        root.contextDlcName = String(dlc.name || "")
        dlcContextMenu.actions = launcherBridge.libraryDlcActions(gameId, root.contextDlcId)
        dlcContextMenu.openAt(item, localX, localY)
    }

    function openDlcBackgroundContext(item, localX, localY) {
        var gameId = root.selectedGame().gameId || ""
        root.contextDlcGameId = gameId
        root.contextDlcId = ""
        root.contextDlcName = ""
        dlcBackgroundMenu.actions = launcherBridge.libraryDlcBackgroundActions(gameId)
        dlcBackgroundMenu.openAt(item, localX, localY)
    }

    function installedDlcCount() {
        var count = 0
        for (var i = 0; i < dlcModel.count; ++i)
            if (dlcModel.get(i).installed) count += 1
        return count
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
                root.fixtureMode = launcherBridge.stringSetting(key, launcherBridge.testMode ? "generic" : "none")
                root.populateBackendLibrary()
            }
        }
        function onLibraryChanged() {
            var previousGameId = root.selectedGame().gameId || ""
            root.populateBackendLibrary()
            if (previousGameId.length > 0) {
                for (var i = 0; i < gamesModel.count; ++i) {
                    if (gamesModel.get(i).gameId === previousGameId) {
                        root.selectedGameIndex = i
                        break
                    }
                }
            }
        }
        function onLibraryDlcChanged(gameId) {
            if (gameId.length === 0 || gameId === (root.selectedGame().gameId || ""))
                root.populateDlcForSelection()
        }
    }

    Component.onCompleted: populateBackendLibrary()

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
                    id: libraryStack
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: root.hasGames ? 1 : 0

                    EmptyState {
                        glyph: "▣"
                        title: root.testMode && root.fixtureMode !== "none" ? "No fixture games loaded" : "Your library is empty"
                        description: "Add your own local game content. Xenon asks installed modules to identify and validate supported files; commercial game data is never distributed by Xenon."
                        primaryText: root.testMode && root.fixtureMode !== "none" ? "Reload Fixture" : "Add Game"
                        secondaryText: ""
                        onPrimaryClicked: root.testMode && root.fixtureMode !== "none" ? root.populateBackendLibrary() : gameContentDialog.open()
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

                // Right-click belongs to the visible Library viewport rather than
                // the ListView contentItem. This keeps row hit-testing correct
                // after scrolling and gives unused/empty library space a target.
                MouseArea {
                    parent: libraryStack
                    anchors.fill: libraryStack
                    z: 1000
                    acceptedButtons: Qt.RightButton
                    hoverEnabled: false
                    preventStealing: true
                    onClicked: function(mouse) {
                        if (libraryStack.currentIndex === 1) {
                            var point = libraryStack.mapToItem(gameList, mouse.x, mouse.y)
                            var index = gameList.indexAt(
                                point.x + gameList.contentX,
                                point.y + gameList.contentY)
                            if (index >= 0) {
                                root.openGameContext(index, libraryStack, mouse.x, mouse.y)
                                return
                            }
                        }
                        root.openLibraryBackgroundContext(libraryStack, mouse.x, mouse.y)
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
                                        onClicked: {
                                            if (gameActionsMenu.visible) {
                                                gameActionsMenu.close()
                                            } else {
                                                gameActionsMenu.actions = launcherBridge.libraryGameActions(root.selectedGame().gameId)
                                                gameActionsMenu.open()
                                            }
                                        }
                                        XActionMenu {
                                            id: gameActionsMenu
                                            parent: gameActionsButton
                                            x: gameActionsButton.width - width
                                            y: gameActionsButton.height + 4
                                            menuWidth: 300
                                            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                            onActionTriggered: function(actionId) {
                                                root.runGameAction(actionId, root.selectedGameIndex)
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
                                    StatusPill {
                                        visible: String(root.selectedGame().compatibilityLabel || "").length > 0
                                        label: String(root.selectedGame().compatibilityLabel || "")
                                        tone: String(root.selectedGame().compatibilityStatus || "").toLowerCase() === "development" ? Theme.warning : Theme.textMuted
                                    }
                                    StatusPill {
                                        visible: root.sessionMatchesSelected() && root.selectedSessionState() !== "idle"
                                        label: String(root.currentSession.stateLabel || "Session")
                                        tone: root.selectedSessionState() === "failed" ? Theme.danger
                                            : root.selectedSessionState() === "running" ? Theme.success : Theme.warning
                                    }
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
                                        text: root.launchButtonText()
                                        variant: root.launchButtonVariant()
                                        enabled: root.launchButtonEnabled()
                                        onClicked: root.activateSelectedGameSession()
                                    }
                                    XButton {
                                        visible: Boolean(root.selectedGame().moduleInstalled) && !Boolean(root.selectedGame().moduleActive)
                                        text: "Enable " + root.selectedGame().moduleName
                                        variant: "primary"
                                        onClicked: launcherBridge.setModuleEnabled(root.selectedGame().moduleId, true)
                                    }
                                    XButton {
                                        id: manageFilesButton
                                        visible: launcherBridge.featureEnabled("library.manageFiles")
                                        text: "Manage Files  ▾"
                                        onClicked: {
                                            if (manageMenu.visible) {
                                                manageMenu.close()
                                            } else {
                                                manageMenu.actions = launcherBridge.libraryManageActions(root.selectedGame().gameId)
                                                manageMenu.open()
                                            }
                                        }
                                        XActionMenu {
                                            id: manageMenu
                                            parent: manageFilesButton
                                            x: 0
                                            y: manageFilesButton.height + 5
                                            menuWidth: 270
                                            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                            onActionTriggered: function(actionId) {
                                                root.runGameAction(actionId, root.selectedGameIndex)
                                            }
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    visible: root.sessionMatchesSelected() && root.selectedSessionState() !== "idle"
                                    implicitHeight: sessionStateColumn.implicitHeight + Theme.spaceMd * 2
                                    radius: Theme.controlRadius
                                    color: root.selectedSessionState() === "failed" ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.08)
                                        : Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.07)
                                    border.width: Theme.borderWidth
                                    border.color: root.selectedSessionState() === "failed" ? Theme.danger : Theme.border

                                    ColumnLayout {
                                        id: sessionStateColumn
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.top: parent.top
                                        anchors.margins: Theme.spaceMd
                                        spacing: Theme.spaceXs

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                Layout.fillWidth: true
                                                text: root.selectedSessionState() === "failed"
                                                    ? String((root.currentSession.error || {}).title || "Launch failed")
                                                    : String(root.currentSession.stateLabel || "Session")
                                                color: root.selectedSessionState() === "failed" ? Theme.danger : Theme.text
                                                font.pixelSize: Theme.typeBody
                                                font.weight: Font.DemiBold
                                            }
                                            Text {
                                                visible: root.selectedSessionState() === "running"
                                                text: root.formatDuration(root.currentSession.elapsedMs)
                                                color: Theme.textMuted
                                                font.pixelSize: Theme.typeCaption
                                            }
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            visible: root.selectedSessionState() === "failed"
                                            text: String((root.currentSession.error || {}).message || "The game session could not be started.")
                                            color: Theme.textMuted
                                            wrapMode: Text.WordWrap
                                            font.pixelSize: Theme.typeCaption
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            visible: root.selectedSessionState() === "failed"
                                                && String((root.currentSession.error || {}).code || "").length > 0
                                            text: "Stage: " + String((root.currentSession.error || {}).code || "")
                                            color: Theme.textMuted
                                            font.pixelSize: Theme.typeCaption
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            visible: root.selectedSessionState() !== "failed" && root.selectedSessionState() !== "running"
                                            text: root.selectedSessionState() === "stopping"
                                                ? "The launcher is waiting for the current session to stop cleanly."
                                                : "Xenon is progressing through the launcher-side session pipeline. You can cancel before execution begins."
                                            color: Theme.textMuted
                                            wrapMode: Text.WordWrap
                                            font.pixelSize: Theme.typeCaption
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            visible: root.selectedSessionState() === "failed"
                                            Item { Layout.fillWidth: true }
                                            XButton {
                                                text: "Dismiss"
                                                variant: "ghost"
                                                onClicked: launcherBridge.dismissSessionFailure()
                                            }
                                            XButton {
                                                text: "Try Again"
                                                variant: "primary"
                                                onClicked: launcherBridge.launchGame(root.selectedGame().gameId)
                                            }
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
                                        XButton {
                                            text: "+  Import DLC"
                                            onClicked: {
                                                root.pendingImportDlcId = ""
                                                importDlcDialog.open()
                                            }
                                        }
                                    }
                                }

                                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                                Item {
                                    id: dlcViewport
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
                                            required property string dlcId
                                            required property string name
                                            required property string description
                                            required property bool installed
                                            required property string state
                                            required property string path
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
                                                    Layout.maximumWidth: Math.max(92, parent.width * 0.30)
                                                    text: state || (installed ? "Installed" : "Not installed")
                                                    color: installed ? Theme.success : Theme.textMuted
                                                    font.pixelSize: Theme.typeCaption
                                                    elide: Text.ElideRight
                                                }
                                                XIconButton {
                                                    id: dlcActionsButton
                                                    iconName: "more"
                                                    tooltip: "DLC actions"
                                                    variant: "ghost"
                                                    onClicked: {
                                                        if (dlcActionsMenu.visible) {
                                                            dlcActionsMenu.close()
                                                        } else {
                                                            dlcActionsMenu.actions = launcherBridge.libraryDlcActions(root.selectedGame().gameId, dlcId)
                                                            dlcActionsMenu.open()
                                                        }
                                                    }
                                                    XActionMenu {
                                                        id: dlcActionsMenu
                                                        parent: dlcActionsButton
                                                        x: dlcActionsButton.width - width
                                                        y: dlcActionsButton.height + 4
                                                        menuWidth: 230
                                                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                                        onActionTriggered: function(actionId) {
                                                            root.runDlcAction(actionId, root.selectedGame().gameId, dlcId, name)
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    MouseArea {
                                        parent: dlcViewport
                                        anchors.fill: dlcViewport
                                        z: 1000
                                        acceptedButtons: Qt.RightButton
                                        hoverEnabled: false
                                        preventStealing: true
                                        onClicked: function(mouse) {
                                            var point = dlcViewport.mapToItem(dlcList, mouse.x, mouse.y)
                                            var index = dlcList.indexAt(
                                                point.x + dlcList.contentX,
                                                point.y + dlcList.contentY)
                                            if (index >= 0)
                                                root.openDlcContext(index, dlcViewport, mouse.x, mouse.y)
                                            else
                                                root.openDlcBackgroundContext(dlcViewport, mouse.x, mouse.y)
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
                                XInfoRow { visible: String(root.selectedGame().titleId || "").length > 0; label: "Xbox Title ID"; value: String(root.selectedGame().titleId || "") }
                                XInfoRow { visible: String(root.selectedGame().gameDeveloper || "").length > 0; label: "Developer"; value: String(root.selectedGame().gameDeveloper || "") }
                                XInfoRow { visible: String(root.selectedGame().gamePublisher || "").length > 0; label: "Publisher"; value: String(root.selectedGame().gamePublisher || "") }
                                XInfoRow { visible: String(root.selectedGame().gamePlatform || "").length > 0; label: "Platform"; value: String(root.selectedGame().gamePlatform || "") }
                                XInfoRow { visible: Number(root.selectedGame().releaseYear || 0) > 0; label: "Released"; value: String(root.selectedGame().releaseYear || "") }
                                XInfoRow { label: "Module"; value: root.selectedGame().moduleName }
                                XInfoRow { label: "Module version"; value: root.selectedGame().moduleVersion || "Module defined" }
                                XInfoRow { label: "Runtime"; value: "Xenon Recomp" }
                                XInfoRow { label: "Renderer"; value: root.selectedGame().renderer }
                                XInfoRow { label: "Mode"; value: root.selectedGame().mode }
                                XInfoRow { label: "Regions"; value: root.selectedGame().regions }
                                XInfoRow { label: "Content state"; value: root.selectedGame().contentState }
                                XInfoRow { label: "Last played"; value: root.selectedGame().lastPlayed || "Not recorded" }
                                XInfoRow { label: "Play count"; value: String(root.selectedGame().playCount || 0) }
                                XInfoRow { label: "Total play time"; value: root.formatDuration(root.selectedGame().totalPlayTimeMs || 0) }
                                XInfoRow { label: "Last session"; value: root.selectedGame().lastSessionOutcome ? String(root.selectedGame().lastSessionOutcome) : "Not recorded" }
                                XInfoRow { label: "Status"; value: String(root.selectedGame().compatibilityLabel || "").length > 0 ? String(root.selectedGame().compatibilityLabel) : (root.selectedGame().ready ? "Launch ready" : root.selectedGame().status) }
                                XInfoRow { visible: Boolean(root.selectedGame().metadataAvailable); label: "Metadata"; value: String(root.selectedGame().metadataSource || "GitHub") }
                                XInfoRow {
                                    visible: Boolean(root.selectedGame().metadataAvailable)
                                    label: "Registry sync"
                                    value: String(root.selectedGame().metadataStatus || "ready")
                                }
                                XInfoRow {
                                    visible: Boolean(root.selectedGame().metadataAvailable) && String(root.selectedGame().metadataLastCheckedAt || "").length > 0
                                    label: "Last metadata check"
                                    value: String(root.selectedGame().metadataLastCheckedAt || "")
                                }
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
                                XInfoRow {
                                    label: "Game launch"
                                    value: root.sessionMatchesSelected() && root.selectedSessionState() !== "idle"
                                        ? String(root.currentSession.stateLabel || "Session")
                                        : root.selectedGame().ready ? (launcherBridge.backendConnected ? "Launch contract ready" : "Runtime unavailable") : "Unavailable"
                                }
                                XInfoRow { label: "Audio"; value: "Backend pending" }
                                XInfoRow { label: "Online features"; value: "Not implemented" }
                                XInfoRow { label: "Achievements"; value: "Not implemented" }
                                XInfoRow {
                                    visible: String(root.selectedGame().compatibilityLabel || "").length > 0
                                    label: "Registry compatibility"
                                    value: String(root.selectedGame().compatibilityLabel || "")
                                }
                                Text {
                                    visible: String(root.selectedGame().compatibilitySummary || "").length > 0
                                    Layout.fillWidth: true
                                    text: String(root.selectedGame().compatibilitySummary || "")
                                    color: Theme.textMuted
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: Theme.typeCaption
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.testMode ? "Fixture content is active, while presentation metadata and the DLC catalogue can be refreshed from the official Xenon Modules registry." : "Library, module and DLC state is launcher-core backed. Presentation metadata is refreshed from the official Xenon Modules registry."
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

    XActionMenu {
        id: libraryContextMenu
        parent: root
        menuWidth: 310
        onActionTriggered: function(actionId) {
            root.runGameAction(actionId, root.contextGameIndex)
        }
    }

    XActionMenu {
        id: libraryBackgroundMenu
        parent: root
        menuWidth: 230
        onActionTriggered: function(actionId) {
            root.runLibraryBackgroundAction(actionId)
        }
    }

    XActionMenu {
        id: dlcContextMenu
        parent: root
        menuWidth: 250
        onActionTriggered: function(actionId) {
            root.runDlcAction(actionId, root.contextDlcGameId, root.contextDlcId, root.contextDlcName)
        }
    }

    XActionMenu {
        id: dlcBackgroundMenu
        parent: root
        menuWidth: 230
        onActionTriggered: function(actionId) {
            if (actionId === "import") {
                root.pendingImportDlcId = ""
                importDlcDialog.open()
            }
        }
    }

    // Keep the properties surface out of the Library startup path. It is loaded
    // only when requested, so a dialog-specific regression cannot prevent the
    // launcher shell from opening.
    Loader {
        id: gamePropertiesLoader
        active: false
        asynchronous: true
        sourceComponent: Component { GamePropertiesDialog {} }
    }

    XConfirmDialog {
        id: removeGameConfirm
        confirmText: "Remove from Library"
        destructive: true
        onConfirmed: {
            launcherBridge.removeLibraryEntry(root.pendingRemoveGameId)
            root.pendingRemoveGameId = ""
        }
    }

    XConfirmDialog {
        id: removeDlcConfirm
        title: "Remove “" + root.pendingDlcName + "”?"
        message: "This deletes the local copy inside Xenon's managed DLC folder. It does not touch the original package you imported from elsewhere."
        confirmText: "Remove DLC"
        destructive: true
        onConfirmed: {
            launcherBridge.removeLibraryDlc(root.pendingDlcGameId, root.pendingDlcId)
            root.pendingDlcGameId = ""
            root.pendingDlcId = ""
            root.pendingDlcName = ""
        }
    }

    FileDialog {
        id: importDlcDialog
        title: "Import DLC from local files"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["All files (*)"]
        onAccepted: {
            if (root.pendingImportDlcId.length > 0)
                launcherBridge.importDlcContentForEntry(root.selectedGame().gameId, root.pendingImportDlcId, selectedFiles)
            else
                launcherBridge.importDlcContent(root.selectedGame().gameId, selectedFiles)
            root.pendingImportDlcId = ""
        }
        onRejected: root.pendingImportDlcId = ""
    }

    FileDialog {
        id: gameContentDialog
        title: "Add game from local content"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["All files (*)"]
        onAccepted: {
            if (launcherBridge.importGameContent(selectedFiles))
                root.populateBackendLibrary()
        }
    }

    ModuleSettingsDialog {
        id: moduleSettingsDialog
        onSettingEdited: function(settingId, value) {
            launcherBridge.setModuleSetting(root.selectedGame().moduleId || "", settingId, value)
        }
    }
}
