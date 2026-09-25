import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

Item {
    id: root

    property string searchText: ""
    property string localSearchText: ""
    property int selectedGameIndex: -1
    property bool detailOpen: false
    property int modelRevision: 0
    property bool wrapNavigation: launcherBridge.boolSetting("library/wrapNavigation", true)
    property bool rememberSelection: launcherBridge.boolSetting("library/rememberSelection", true)
    property string pendingSelectionGameId: ""
    property string lastDetailGameId: ""
    property var detailScrollByGame: ({})
    property var dlcCache: ({})
    property var dlcCacheOrder: []
    property int dlcCacheLimit: 12
    property bool showModuleArtwork: launcherBridge.boolSetting("appearance/artworkBackgrounds", true)
    property bool showCompatibility: launcherBridge.boolSetting("library/showCompatibility", true)
    property string fixtureMode: launcherBridge.stringSetting(
        "developer/fixtureMode", launcherBridge.testMode ? "generic" : "none")
    property string libraryFilter: launcherBridge.stringSetting("library/filter", "All Games")
    property string librarySort: launcherBridge.stringSetting("library/sort", "Recently Played")
    property string libraryView: launcherBridge.stringSetting("library/viewMode", "Focused")
    property string gridDensity: launcherBridge.stringSetting("library/gridDensity", "Auto")
    property var sourceGames: []
    property var favoriteLookup: ({})
    property string pendingDlcGameId: ""
    property string pendingDlcId: ""
    property string pendingDlcName: ""
    property string pendingImportDlcId: ""
    property var pendingGameImportFiles: []
    property string pendingRemoveGameId: ""
    property string pendingDeleteManagedGameId: ""
    property string pendingDeleteManagedGameTitle: ""
    property int contextGameIndex: -1
    property string contextDlcGameId: ""
    property string contextDlcId: ""
    property string contextDlcName: ""

    signal requestPage(int index)

    readonly property bool hasGames: gamesModel.count > 0
    readonly property bool testMode: launcherBridge.testMode
    readonly property var currentSession: launcherBridge.currentSession
    readonly property var currentGame: {
        var revision = root.modelRevision
        if (!root.hasGames || root.selectedGameIndex < 0 || root.selectedGameIndex >= gamesModel.count)
            return root.emptyGame()
        return gamesModel.get(root.selectedGameIndex)
    }
    readonly property int gridColumnCount: {
        var available = Math.max(1, libraryBrowseStack.width)
        var capacity = Math.max(1, Math.floor(available / 118))
        if (root.gridDensity === "3") return Math.max(1, Math.min(3, capacity))
        if (root.gridDensity === "6") return Math.max(1, Math.min(6, capacity))
        return Math.max(2, Math.min(6, Math.floor(available / 150)))
    }

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
            tileArtFocalX: 0.5, tileArtFocalY: 0.5, heroArtFocalX: 0.5, heroArtFocalY: 0.5,
            gameId: "", titleId: "", renderer: "", mode: "", regions: "", contentState: "",
            tags: "", moduleVersion: "", lastPlayed: "",
            gameDeveloper: "", gamePublisher: "", gamePlatform: "", releaseYear: 0,
            compatibilityStatus: "", compatibilityLabel: "", compatibilitySummary: "",
            metadataAvailable: false, metadataSource: "", metadataStatus: "",
            metadataStatusMessage: "", metadataLastCheckedAt: "", favorite: false,
            playCount: 0, totalPlayTimeMs: 0, lastPlayedAt: "", addedAt: "",
            contentExists: false, moduleId: "", moduleInstalled: false, moduleActive: false,
            moduleUpdateAvailable: false, moduleUpdateStatus: "idle"
        }
    }

    function selectedGame() {
        return root.currentGame
    }

    function openGameDetails(index) {
        if (index < 0 || index >= gamesModel.count) return
        root.selectedGameIndex = index
        root.detailOpen = true
        root.populateDlcForSelection()
        Qt.callLater(function() {
            if (detailScroll && detailScroll.contentItem)
                detailScroll.contentItem.contentY = Number(root.detailScrollByGame[root.selectedGame().gameId] || 0)
        })
    }

    function closeGameDetails() {
        root.detailOpen = false
        Qt.callLater(root.focusSelectedGame)
    }

    function handleBackNavigation() {
        if (!root.detailOpen) return false
        root.closeGameDetails()
        return true
    }

    function selectGameById(gameId, showDetails) {
        var target = String(gameId || "")
        if (target.length === 0) return false
        for (var i = 0; i < gamesModel.count; ++i) {
            if (String(gamesModel.get(i).gameId || "") === target) {
                selectedGameIndex = i
                if (showDetails !== false) detailOpen = true
                return true
            }
        }
        return false
    }

    function gameAt(index) {
        if (index < 0 || index >= gamesModel.count) return emptyGame()
        return gamesModel.get(index)
    }

    function isFavorite(gameId) {
        var target = String(gameId || "")
        if (target.length === 0) return false
        return Boolean(root.favoriteLookup[target])
    }

    function toggleFavorite(gameId) {
        var target = String(gameId || "")
        if (target.length === 0) return
        var nextValue = !root.isFavorite(target)
        if (!launcherBridge.setLibraryFavorite(target, nextValue)) return

        // Update the small lookup immediately so the star responds in the
        // same frame even if storage/model refresh arrives a moment later.
        var nextLookup = Object.assign({}, root.favoriteLookup)
        nextLookup[target] = nextValue
        root.favoriteLookup = nextLookup
        for (var i = 0; i < gamesModel.count; ++i) {
            if (String(gamesModel.get(i).gameId || "") === target) {
                gamesModel.setProperty(i, "favorite", nextValue)
                break
            }
        }
        root.modelRevision += 1
        if (root.libraryFilter === "Favorites" || root.librarySort === "Favorites First")
            root.rebuildLibrary(nextValue ? target : "")
    }

    function effectiveSearchText() {
        var local = root.localSearchText.trim()
        return local.length > 0 ? local : root.searchText.trim()
    }

    function matchesSearch(game) {
        var needle = root.effectiveSearchText().toLowerCase()
        if (needle.length === 0) return true
        var haystack = [game.title, game.moduleName, game.gameDeveloper, game.gamePublisher,
                        game.tags, game.gameId, game.titleId].join(" ").toLowerCase()
        var tokens = needle.split(/\s+/)
        for (var i = 0; i < tokens.length; ++i) {
            if (tokens[i].length > 0 && haystack.indexOf(tokens[i]) === -1) return false
        }
        return true
    }

    function matchesFilter(game) {
        if (root.libraryFilter === "Installed") {
            // "installed" historically means "assigned module is installed" in
            // LibraryFeature. For the user-facing Library filter we mean the
            // actual registered game content exists. Older/fixture rows that do
            // not expose contentExists fall back to the legacy role.
            return game.contentExists === undefined ? Boolean(game.installed) : Boolean(game.contentExists)
        }
        if (root.libraryFilter === "Ready to Play") return Boolean(game.ready)
        if (root.libraryFilter === "Needs Attention") return !Boolean(game.ready) || Boolean(game.moduleUpdateAvailable)
        if (root.libraryFilter === "Module Disabled") return Boolean(game.moduleInstalled) && !Boolean(game.moduleActive)
        if (root.libraryFilter === "Update Available") return Boolean(game.moduleUpdateAvailable)
        if (root.libraryFilter === "Favorites") return root.isFavorite(game.gameId)
        return true
    }

    function compareGames(a, b) {
        var at = String(a.title || "").toLocaleLowerCase()
        var bt = String(b.title || "").toLocaleLowerCase()
        if (root.librarySort === "A-Z") return at.localeCompare(bt)
        if (root.librarySort === "Z-A") return bt.localeCompare(at)
        if (root.librarySort === "Playtime: High to Low")
            return Number(b.totalPlayTimeMs || 0) - Number(a.totalPlayTimeMs || 0) || at.localeCompare(bt)
        if (root.librarySort === "Playtime: Low to High")
            return Number(a.totalPlayTimeMs || 0) - Number(b.totalPlayTimeMs || 0) || at.localeCompare(bt)
        if (root.librarySort === "Recently Added") {
            var aa = String(a.addedAt || "")
            var ba = String(b.addedAt || "")
            return ba.localeCompare(aa) || at.localeCompare(bt)
        }
        if (root.librarySort === "Favorites First") {
            var af = root.isFavorite(a.gameId) ? 1 : 0
            var bf = root.isFavorite(b.gameId) ? 1 : 0
            return bf - af || at.localeCompare(bt)
        }
        if (root.librarySort === "Updates First") {
            var au = Boolean(a.moduleUpdateAvailable) ? 1 : 0
            var bu = Boolean(b.moduleUpdateAvailable) ? 1 : 0
            return bu - au || at.localeCompare(bt)
        }
        // Backend lastPlayed is kept as an ISO-ish display string today.
        // Lexical ordering works for the normal stored format; unknown values
        // naturally fall behind real timestamps.
        var al = String(a.lastPlayedAt || "")
        var bl = String(b.lastPlayedAt || "")
        return bl.localeCompare(al) || at.localeCompare(bt)
    }

    function currentBrowsePosition() {
        if (root.libraryView === "Carousel")
            return ({ view: "Carousel", offset: carouselView.contentX })
        if (root.libraryView === "Grid")
            return ({ view: "Grid", offset: gridView.contentY })
        return ({ view: root.libraryView, offset: 0 })
    }

    function restoreBrowsePosition(position) {
        if (!position || position.view !== root.libraryView) return
        if (position.view === "Carousel") {
            var maxX = Math.max(0, carouselView.contentWidth - carouselView.width)
            carouselView.contentX = Math.max(0, Math.min(maxX, Number(position.offset || 0)))
        } else if (position.view === "Grid") {
            var maxY = Math.max(0, gridView.contentHeight - gridView.height)
            gridView.contentY = Math.max(0, Math.min(maxY, Number(position.offset || 0)))
        }
    }

    function modelIndexForGameId(gameId, firstIndex) {
        var target = String(gameId || "")
        if (target.length === 0) return -1
        for (var i = Math.max(0, Number(firstIndex || 0)); i < gamesModel.count; ++i) {
            if (String(gamesModel.get(i).gameId || "") === target) return i
        }
        return -1
    }

    // Reconcile the visible model in place. Filtering, sorting and backend
    // refreshes used to clear and append the complete model, which destroyed
    // every delegate (and its decoded artwork) even when a single role had
    // changed. Moves/removals/inserts keep virtualized delegates, selection
    // and scroll state stable for large libraries.
    function syncVisibleGames(visible) {
        var wanted = ({})
        for (var i = 0; i < visible.length; ++i)
            wanted[String(visible[i].gameId || "")] = true

        for (var oldIndex = gamesModel.count - 1; oldIndex >= 0; --oldIndex) {
            if (!wanted[String(gamesModel.get(oldIndex).gameId || "")])
                gamesModel.remove(oldIndex)
        }

        for (var targetIndex = 0; targetIndex < visible.length; ++targetIndex) {
            var desired = visible[targetIndex]
            var desiredId = String(desired.gameId || "")
            var existingIndex = root.modelIndexForGameId(desiredId, targetIndex)
            if (existingIndex < 0) {
                gamesModel.insert(targetIndex, desired)
            } else {
                if (existingIndex !== targetIndex)
                    gamesModel.move(existingIndex, targetIndex, 1)
                // set() changes roles on the existing row instead of
                // replacing the delegate object.
                gamesModel.set(targetIndex, desired)
            }
        }

        while (gamesModel.count > visible.length)
            gamesModel.remove(gamesModel.count - 1)
    }

    function rebuildLibrary(preserveGameId) {
        var keep = String(preserveGameId || (root.selectedGame().gameId || ""))
        var position = root.currentBrowsePosition()
        var previousSelectedId = root.selectedGame().gameId || ""
        var visible = []
        for (var i = 0; i < root.sourceGames.length; ++i) {
            var game = root.sourceGames[i]
            if (root.matchesSearch(game) && root.matchesFilter(game)) visible.push(game)
        }
        visible.sort(root.compareGames)
        root.syncVisibleGames(visible)
        root.modelRevision += 1

        root.selectedGameIndex = gamesModel.count > 0 ? 0 : -1
        if (gamesModel.count === 0) root.detailOpen = false
        var remembered = root.rememberSelection
            ? launcherBridge.stringSetting("library/lastSelectedGameId", "") : ""
        var target = keep.length > 0 ? keep : remembered
        if (target.length > 0) root.selectGameById(target, false)
        if ((root.selectedGame().gameId || "") !== previousSelectedId)
            root.populateDlcForSelection()
        Qt.callLater(function() { root.restoreBrowsePosition(position) })
    }

    function setLibraryFilter(value) {
        root.libraryFilter = value
        launcherBridge.setSettingValue("library/filter", value)
        root.rebuildLibrary(root.selectedGame().gameId || "")
    }

    function setLibrarySort(value) {
        root.librarySort = value
        launcherBridge.setSettingValue("library/sort", value)
        root.rebuildLibrary(root.selectedGame().gameId || "")
    }

    function setLibraryView(value) {
        root.libraryView = value
        launcherBridge.setSettingValue("library/viewMode", value)
        Qt.callLater(root.focusSelectedGame)
    }

    function setGridDensity(value) {
        root.gridDensity = value
        launcherBridge.setSettingValue("library/gridDensity", value)
    }

    function selectRelative(delta) {
        if (gamesModel.count === 0) return
        var next = root.selectedGameIndex + delta
        if (root.wrapNavigation && Math.abs(delta) === 1 && gamesModel.count > 1) {
            if (next < 0) next = gamesModel.count - 1
            else if (next >= gamesModel.count) next = 0
        } else {
            next = Math.max(0, Math.min(gamesModel.count - 1, next))
        }
        if (next === root.selectedGameIndex) return
        root.selectedGameIndex = next
        Qt.callLater(root.focusSelectedGame)
    }

    function selectBoundary(first) {
        if (gamesModel.count === 0) return
        var next = first ? 0 : gamesModel.count - 1
        if (next === root.selectedGameIndex) return
        root.selectedGameIndex = next
        Qt.callLater(root.focusSelectedGame)
    }

    function triggerSecondaryAction() {
        if (root.selectedGame().gameId.length > 0)
            root.toggleFavorite(root.selectedGame().gameId)
    }

    function focusSearch() {
        librarySearch.forceActiveFocus()
        librarySearch.selectAll()
    }

    function focusSelectedGame() {
        if (root.libraryView === "Focused" && focusedGameButton.visible)
            focusedGameButton.forceActiveFocus()
        else if (root.libraryView === "Carousel" && carouselView.visible) {
            carouselView.currentIndex = root.selectedGameIndex
            carouselView.positionViewAtIndex(root.selectedGameIndex, ListView.Center)
            var item = carouselView.itemAtIndex(root.selectedGameIndex)
            if (item) item.forceActiveFocus()
        } else if (root.libraryView === "Grid" && gridView.visible) {
            gridView.currentIndex = root.selectedGameIndex
            gridView.positionViewAtIndex(root.selectedGameIndex, GridView.Contain)
            var gridItem = gridView.itemAtIndex(root.selectedGameIndex)
            if (gridItem) gridItem.forceActiveFocus()
        }
    }

    function handleDirectionalNavigation(direction) {
        if (gamesModel.count === 0) return false
        if (root.libraryView === "Focused") {
            var focusedSelectorActive = focusedGameButton.activeFocus
                || previousGameButton.activeFocus || nextGameButton.activeFocus
            if (!focusedSelectorActive) return false
            if (direction === "up") { root.selectRelative(-1); return true }
            if (direction === "down") { root.selectRelative(1); return true }
            return false
        }
        if (root.libraryView === "Carousel") {
            if (!carouselView.activeFocus) return false
            if (direction === "left") { root.selectRelative(-1); return true }
            if (direction === "right") { root.selectRelative(1); return true }
            return false
        }
        if (root.libraryView === "Grid") {
            if (!gridView.activeFocus) return false
            var step = root.gridColumnCount
            if (direction === "left") { root.selectRelative(-1); return true }
            if (direction === "right") { root.selectRelative(1); return true }
            if (direction === "up") { root.selectRelative(-step); return true }
            if (direction === "down") { root.selectRelative(step); return true }
        }
        return false
    }

    onSearchTextChanged: searchDebounce.restart()
    onLocalSearchTextChanged: searchDebounce.restart()

    Timer {
        id: searchDebounce
        interval: 120
        repeat: false
        onTriggered: root.rebuildLibrary(root.selectedGame().gameId || "")
    }

    Timer {
        id: selectionPersistTimer
        interval: 450
        repeat: false
        onTriggered: {
            if (root.rememberSelection && root.pendingSelectionGameId.length > 0)
                launcherBridge.setSettingValue("library/lastSelectedGameId", root.pendingSelectionGameId)
        }
    }

    function clearLibrary() {
        gamesModel.clear()
        dlcModel.clear()
        selectedGameIndex = -1
        detailOpen = false
    }

    function populateBackendLibrary() {
        var keep = root.selectedGame().gameId || ""
        var items = launcherBridge.libraryEntries()
        var nextGames = []
        var nextFavorites = ({})
        for (var i = 0; i < items.length; ++i) {
            nextGames.push(items[i])
            var gameId = String(items[i].gameId || "")
            if (gameId.length > 0) nextFavorites[gameId] = Boolean(items[i].favorite)
        }
        root.sourceGames = nextGames
        root.favoriteLookup = nextFavorites
        root.rebuildLibrary(keep)
    }

    function moduleSettingsSchema() {
        return launcherBridge.moduleSettingsSchema(root.selectedGame().moduleId || "")
    }

    function populateDlcForSelection(forceRefresh) {
        dlcModel.clear()
        if (selectedGameIndex < 0) return
        var gameId = String(root.selectedGame().gameId || "")
        if (gameId.length === 0) return
        var items = (!forceRefresh && root.dlcCache[gameId] !== undefined)
            ? root.dlcCache[gameId] : launcherBridge.libraryDlcEntries(gameId)

        // Keep a small LRU-style cache. Browsing a very large library should
        // not retain every game's DLC metadata for the lifetime of the page.
        var nextOrder = root.dlcCacheOrder.slice()
        var existingIndex = nextOrder.indexOf(gameId)
        if (existingIndex >= 0) nextOrder.splice(existingIndex, 1)
        nextOrder.push(gameId)

        if (forceRefresh || root.dlcCache[gameId] === undefined) {
            var nextCache = Object.assign({}, root.dlcCache)
            nextCache[gameId] = items
            while (nextOrder.length > Math.max(1, root.dlcCacheLimit)) {
                var evicted = nextOrder.shift()
                delete nextCache[evicted]
            }
            root.dlcCache = nextCache
        } else {
            while (nextOrder.length > Math.max(1, root.dlcCacheLimit))
                nextOrder.shift()
        }
        root.dlcCacheOrder = nextOrder

        for (var i = 0; i < items.length; ++i) dlcModel.append(items[i])
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
        } else if (actionId === "deleteManagedFiles") {
            // Deliberately a separate, stronger confirmation from "remove"
            // above - this one permanently deletes real files, "remove"
            // never does.
            root.pendingDeleteManagedGameId = game.gameId
            root.pendingDeleteManagedGameTitle = game.title
            deleteManagedFilesConfirm.open()
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

    // Reached from Main.qml's Shift+F10/Menu-key shortcut and gamepad Y
    // (Part 17/Part 8's Context action) - opens the exact same menu the
    // visible "more actions" button and right-click already open, so all
    // three input paths share one action model rather than three.
    function openContextMenuForFocusedItem() {
        if (root.selectedGameIndex < 0 || root.selectedGameIndex >= gamesModel.count)
            return
        gameActionsMenu.actions = launcherBridge.libraryGameActions(root.selectedGame().gameId)
        gameActionsMenu.open()
    }

    // Gamepad LT/RT and keyboard PageUp/PageDown (Part 6/19's "page/scroll
    // larger lists") - moves the selected game by a page instead of one row.
    function movePage(forward) {
        if (gamesModel.count === 0) return
        var step = root.libraryView === "Grid" ? Math.max(1, root.gridColumnCount * 2) : 5
        root.selectRelative(forward ? step : -step)
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


    onSelectedGameIndexChanged: {
        if (root.lastDetailGameId.length > 0 && detailScroll) {
            var positions = Object.assign({}, root.detailScrollByGame)
            positions[root.lastDetailGameId] = detailScroll.contentItem ? detailScroll.contentItem.contentY : 0
            root.detailScrollByGame = positions
        }
        root.modelRevision += 1
        root.populateDlcForSelection()
        var gameId = String(root.selectedGame().gameId || "")
        root.pendingSelectionGameId = gameId
        if (root.rememberSelection && gameId.length > 0) selectionPersistTimer.restart()
        root.lastDetailGameId = gameId
        Qt.callLater(function() {
            if (detailScroll && detailScroll.contentItem)
                detailScroll.contentItem.contentY = Number(root.detailScrollByGame[gameId] || 0)
        })
    }

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
            } else if (key === "library/filter") {
                var nextFilter = launcherBridge.stringSetting(key, "All Games")
                if (root.libraryFilter !== nextFilter) {
                    root.libraryFilter = nextFilter
                    root.rebuildLibrary(root.selectedGame().gameId || "")
                }
            } else if (key === "library/sort") {
                var nextSort = launcherBridge.stringSetting(key, "Recently Played")
                if (root.librarySort !== nextSort) {
                    root.librarySort = nextSort
                    root.rebuildLibrary(root.selectedGame().gameId || "")
                }
            } else if (key === "library/viewMode") {
                var nextView = launcherBridge.stringSetting(key, "Focused")
                if (root.libraryView !== nextView) {
                    root.libraryView = nextView
                    Qt.callLater(root.focusSelectedGame)
                }
            } else if (key === "library/gridDensity") {
                root.gridDensity = launcherBridge.stringSetting(key, "Auto")
            } else if (key === "library/wrapNavigation") {
                root.wrapNavigation = launcherBridge.boolSetting(key, true)
            } else if (key === "library/rememberSelection") {
                root.rememberSelection = launcherBridge.boolSetting(key, true)
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
            if (gameId.length === 0) {
                root.dlcCache = ({})
                root.dlcCacheOrder = []
                root.populateDlcForSelection(true)
                return
            }
            var nextCache = Object.assign({}, root.dlcCache)
            delete nextCache[gameId]
            root.dlcCache = nextCache
            var nextOrder = root.dlcCacheOrder.slice()
            var orderIndex = nextOrder.indexOf(gameId)
            if (orderIndex >= 0) nextOrder.splice(orderIndex, 1)
            root.dlcCacheOrder = nextOrder
            if (gameId === (root.selectedGame().gameId || ""))
                root.populateDlcForSelection(true)
        }
    }

    Shortcut {
        sequence: "F"
        enabled: root.visible && root.hasGames && !librarySearch.activeFocus
        onActivated: root.triggerSecondaryAction()
    }

    Shortcut {
        sequence: "Home"
        enabled: root.visible && !librarySearch.activeFocus
        onActivated: root.selectBoundary(true)
    }

    Shortcut {
        sequence: "End"
        enabled: root.visible && !librarySearch.activeFocus
        onActivated: root.selectBoundary(false)
    }

    Component.onCompleted: populateBackendLibrary()

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        XPanel {
            id: librarySelectorPanel
            Layout.fillWidth: !root.detailOpen
            Layout.preferredWidth: root.detailOpen
                ? (root.libraryView === "Grid"
                    ? Math.max(440, Math.min(760, parent.width * 0.47))
                    : Math.max(300, Math.min(400, parent.width * 0.30)))
                : parent.width
            Layout.minimumWidth: root.detailOpen ? (root.libraryView === "Grid" ? 400 : 280) : 0
            Layout.fillHeight: true
            clip: true
            decorated: root.detailOpen
            border.width: root.detailOpen ? Theme.borderWidth : 0
            panelOpacity: root.detailOpen ? Theme.panelOpacity : Math.max(0.28, Theme.panelOpacity * 0.42)

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceSm

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    Text {
                        Layout.fillWidth: true
                        text: "Library"
                        color: Theme.text
                        font.pixelSize: Theme.typeSubtitle
                        font.weight: Font.DemiBold
                    }
                    StatusPill {
                        visible: root.testMode && root.fixtureMode !== "none"
                        label: root.fixtureMode === "gracemeria" ? "GRACEMERIA" : "TEST"
                        tone: Theme.warning
                    }
                    Text {
                        text: gamesModel.count.toString()
                        color: Theme.textMuted
                        font.pixelSize: Theme.typeCaption
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXs

                    XTextField {
                        id: librarySearch
                        Layout.fillWidth: true
                        placeholderText: "Search this library…"
                        text: root.localSearchText
                        accessibleName: "Search library"
                        accessibleDescription: "Filter the current library by game, module, developer, publisher, tag or ID."
                        onTextEdited: root.localSearchText = text
                        Keys.onEscapePressed: function(event) {
                            if (text.length > 0) {
                                clear()
                                root.localSearchText = ""
                                event.accepted = true
                            }
                        }
                    }
                    XIconButton {
                        visible: root.localSearchText.length > 0
                        iconName: "close"
                        tooltip: "Clear library search"
                        variant: "ghost"
                        onClicked: {
                            librarySearch.clear()
                            root.localSearchText = ""
                            librarySearch.forceActiveFocus()
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm

                    XComboBox {
                        id: filterCombo
                        Layout.fillWidth: true
                        Layout.preferredWidth: 150
                        model: ["All Games", "Installed", "Ready to Play", "Needs Attention", "Module Disabled", "Update Available", "Favorites"]
                        currentIndex: Math.max(0, model.indexOf(root.libraryFilter))
                        accessibleName: "Library filter"
                        onActivated: root.setLibraryFilter(String(currentText))
                    }

                    XComboBox {
                        id: sortCombo
                        Layout.fillWidth: true
                        Layout.preferredWidth: 160
                        model: ["Recently Played", "Recently Added", "A-Z", "Z-A", "Playtime: High to Low", "Playtime: Low to High", "Favorites First", "Updates First"]
                        currentIndex: Math.max(0, model.indexOf(root.librarySort))
                        accessibleName: "Library sort order"
                        onActivated: root.setLibrarySort(String(currentText))
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXs

                    Text {
                        Layout.fillWidth: true
                        text: root.libraryView === "Focused" ? "Focused" : root.libraryView
                        color: Theme.textMuted
                        font.pixelSize: Theme.typeCaption
                    }

                    XIconButton {
                        glyph: "☷"
                        tooltip: "Focused vertical view"
                        variant: root.libraryView === "Focused" ? "filled" : "ghost"
                        onClicked: root.setLibraryView("Focused")
                    }
                    XIconButton {
                        glyph: "↔"
                        tooltip: "Horizontal carousel view"
                        variant: root.libraryView === "Carousel" ? "filled" : "ghost"
                        onClicked: root.setLibraryView("Carousel")
                    }
                    XIconButton {
                        glyph: "▦"
                        tooltip: "Artwork grid view"
                        variant: root.libraryView === "Grid" ? "filled" : "ghost"
                        onClicked: root.setLibraryView("Grid")
                    }

                    XComboBox {
                        visible: root.libraryView === "Grid"
                        Layout.preferredWidth: 94
                        model: ["Auto", "3", "6"]
                        currentIndex: Math.max(0, model.indexOf(root.gridDensity))
                        accessibleName: "Grid density"
                        onActivated: root.setGridDensity(String(currentText))
                    }
                }

                StackLayout {
                    id: libraryBrowseStack
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: !root.hasGames ? 0
                        : root.libraryView === "Carousel" ? 2
                        : root.libraryView === "Grid" ? 3 : 1

                    EmptyState {
                        glyph: "▣"
                        title: root.effectiveSearchText().length > 0 ? "No matching games" : "Your library is empty"
                        description: root.effectiveSearchText().length > 0
                            ? "Try another search or clear the current library filter."
                            : "Install a game module from Modules, then add compatible local game content. Xenon never distributes commercial game data."
                        primaryText: root.effectiveSearchText().length > 0
                            ? (root.libraryFilter !== "All Games" ? "Show All Games" : "")
                            : "Open Modules"
                        secondaryText: ""
                        onPrimaryClicked: {
                            if (root.effectiveSearchText().length > 0 && root.libraryFilter !== "All Games")
                                root.setLibraryFilter("All Games")
                            else if (root.effectiveSearchText().length === 0)
                                root.requestPage(1)
                        }
                    }

                    // Focused vertical selector: the selected game is intentionally
                    // dominant. Only the neighbouring game(s) peek into the viewport,
                    // fading toward the edges. If the selected game is first, the
                    // space above remains empty rather than inventing another card.
                    Item {
                        id: focusedSelector
                        clip: true

                        Button {
                            id: previousGameButton
                            visible: root.selectedGameIndex > 0
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: Math.min(126, parent.height * 0.19)
                            opacity: 0.32
                            focusPolicy: Qt.StrongFocus
                            hoverEnabled: true
                            Accessible.name: visible ? "Previous game, " + root.gameAt(root.selectedGameIndex - 1).title : ""
                            onClicked: root.selectRelative(-1)
                            Keys.onDownPressed: function(event) { root.selectRelative(1); event.accepted = true }
                            Keys.onUpPressed: function(event) { root.selectRelative(-1); event.accepted = true }

                            contentItem: Item {
                                ArtworkFrame {
                                    anchors.fill: parent
                                    source: root.showModuleArtwork ? root.gameAt(root.selectedGameIndex - 1).tileArt : ""
                                    focalX: root.gameAt(root.selectedGameIndex - 1).tileArtFocalX
                                    focalY: root.gameAt(root.selectedGameIndex - 1).tileArtFocalY
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    color: Qt.rgba(Theme.window.r, Theme.window.g, Theme.window.b, 0.36)
                                }
                                Text {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: Theme.spaceMd
                                    text: root.gameAt(root.selectedGameIndex - 1).title
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeCaption
                                    elide: Text.ElideRight
                                }
                            }
                            background: Rectangle {
                                radius: Theme.panelRadius
                                color: "transparent"
                                border.width: previousGameButton.activeFocus ? Theme.focusWidth : 0
                                border.color: Theme.focusRing
                            }
                        }

                        Button {
                            id: focusedGameButton
                            objectName: "library-focused-game"
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            height: Math.max(280, Math.min(440, parent.height * 0.62))
                            hoverEnabled: true
                            focusPolicy: Qt.StrongFocus
                            Accessible.name: root.selectedGame().title
                            Accessible.description: root.selectedGame().moduleName + ", " + root.selectedGame().status
                            onClicked: { root.openGameDetails(root.selectedGameIndex); forceActiveFocus() }
                            Keys.onUpPressed: function(event) { root.selectRelative(-1); event.accepted = true }
                            Keys.onDownPressed: function(event) { root.selectRelative(1); event.accepted = true }
                            Keys.onReturnPressed: function(event) { root.openGameDetails(root.selectedGameIndex); event.accepted = true }
                            Keys.onEnterPressed: function(event) { root.openGameDetails(root.selectedGameIndex); event.accepted = true }

                            contentItem: Item {
                                clip: true

                                ArtworkFrame {
                                    anchors.fill: parent
                                    source: root.showModuleArtwork ? root.selectedGame().tileArt : ""
                                    focalX: root.selectedGame().tileArtFocalX
                                    focalY: root.selectedGame().tileArtFocalY
                                }

                                StatusPill {
                                    visible: Boolean(root.selectedGame().moduleUpdateAvailable)
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spaceSm
                                    label: "UPDATE"
                                    tone: Theme.warning
                                }

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: Math.min(parent.height * 0.48, 190)
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "transparent" }
                                        GradientStop { position: 0.45; color: Qt.rgba(Theme.window.r, Theme.window.g, Theme.window.b, 0.72) }
                                        GradientStop { position: 1.0; color: Qt.rgba(Theme.window.r, Theme.window.g, Theme.window.b, 0.96) }
                                    }
                                }

                                ColumnLayout {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.margins: Theme.spaceLg
                                    spacing: Theme.spaceXs

                                    Text {
                                        Layout.fillWidth: true
                                        text: root.selectedGame().title
                                        color: Theme.text
                                        wrapMode: Text.WordWrap
                                        maximumLineCount: 2
                                        elide: Text.ElideRight
                                        font.pixelSize: Theme.typeSubtitle
                                        font.weight: Font.DemiBold
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spaceSm
                                        Rectangle {
                                            width: 10; height: 10; radius: 5
                                            color: root.selectedGame().ready ? Theme.success
                                                : root.selectedGame().installed ? Theme.warning : Theme.textMuted
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: root.selectedGame().status
                                            color: root.selectedGame().ready ? Theme.success : Theme.textMuted
                                            font.pixelSize: Theme.typeCaption
                                            elide: Text.ElideRight
                                        }
                                        Text {
                                            text: root.isFavorite(root.selectedGame().gameId) ? "★" : ""
                                            color: Theme.accent
                                            font.pixelSize: Theme.typeBodyLarge
                                        }
                                    }
                                }
                            }

                            background: Rectangle {
                                radius: Theme.panelRadius
                                color: "transparent"
                                border.width: focusedGameButton.activeFocus ? Theme.focusWidth : Theme.borderWidth * 2
                                border.color: focusedGameButton.activeFocus ? Theme.focusRing : Theme.accent
                            }

                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: function(eventPoint, button) {
                                    root.openGameContext(root.selectedGameIndex, focusedGameButton,
                                                         eventPoint.position.x, eventPoint.position.y)
                                }
                            }
                        }

                        Button {
                            id: nextGameButton
                            visible: root.selectedGameIndex >= 0 && root.selectedGameIndex < gamesModel.count - 1
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: Math.min(150, parent.height * 0.23)
                            opacity: 0.40
                            focusPolicy: Qt.StrongFocus
                            hoverEnabled: true
                            Accessible.name: visible ? "Next game, " + root.gameAt(root.selectedGameIndex + 1).title : ""
                            onClicked: root.selectRelative(1)
                            Keys.onUpPressed: function(event) { root.selectRelative(-1); event.accepted = true }
                            Keys.onDownPressed: function(event) { root.selectRelative(1); event.accepted = true }

                            contentItem: Item {
                                ArtworkFrame {
                                    anchors.fill: parent
                                    source: root.showModuleArtwork ? root.gameAt(root.selectedGameIndex + 1).tileArt : ""
                                    focalX: root.gameAt(root.selectedGameIndex + 1).tileArtFocalX
                                    focalY: root.gameAt(root.selectedGameIndex + 1).tileArtFocalY
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: Qt.rgba(Theme.window.r, Theme.window.g, Theme.window.b, 0.24) }
                                        GradientStop { position: 1.0; color: Qt.rgba(Theme.window.r, Theme.window.g, Theme.window.b, 0.82) }
                                    }
                                }
                                Text {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: Theme.spaceMd
                                    text: root.gameAt(root.selectedGameIndex + 1).title
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.typeCaption
                                    elide: Text.ElideRight
                                }
                            }
                            background: Rectangle {
                                radius: Theme.panelRadius
                                color: "transparent"
                                border.width: nextGameButton.activeFocus ? Theme.focusWidth : 0
                                border.color: Theme.focusRing
                            }
                        }
                    }

                    // Optional console/Switch-style horizontal browsing mode.
                    ListView {
                        id: carouselView
                        orientation: ListView.Horizontal
                        clip: true
                        model: gamesModel
                        spacing: Theme.spaceMd
                        snapMode: ListView.SnapToItem
                        boundsBehavior: Flickable.StopAtBounds
                        preferredHighlightBegin: width * 0.19
                        preferredHighlightEnd: width * 0.81
                        highlightRangeMode: ListView.ApplyRange
                        currentIndex: root.selectedGameIndex
                        cacheBuffer: Math.max(width, 640)
                        reuseItems: true
                        highlightMoveDuration: Theme.reduceMotion ? 0 : 150
                        highlightResizeDuration: Theme.reduceMotion ? 0 : 120
                        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AlwaysOff }
                        onCurrentIndexChanged: {
                            if (currentIndex >= 0 && currentIndex < gamesModel.count && root.selectedGameIndex !== currentIndex)
                                root.selectedGameIndex = currentIndex
                        }

                        delegate: Button {
                            id: carouselCard
                            required property int index
                            required property string title
                            required property string moduleName
                            required property string status
                            required property bool ready
                            required property bool installed
                            required property bool moduleUpdateAvailable
                            required property string tileArt
                            required property real tileArtFocalX
                            required property real tileArtFocalY
                            width: Math.max(170, Math.min(224, carouselView.width * 0.64))
                            height: Math.max(260, carouselView.height - Theme.spaceLg * 2)
                            scale: root.selectedGameIndex === index ? 1.0 : 0.90
                            opacity: root.selectedGameIndex === index ? 1.0 : 0.52
                            Behavior on scale {
                                enabled: !Theme.reduceMotion
                                NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
                            }
                            Behavior on opacity {
                                enabled: !Theme.reduceMotion
                                NumberAnimation { duration: 100 }
                            }
                            focusPolicy: Qt.StrongFocus
                            hoverEnabled: true
                            Accessible.name: title
                            onClicked: { root.openGameDetails(index); forceActiveFocus() }
                            onActiveFocusChanged: if (activeFocus) root.selectedGameIndex = index
                            Keys.onLeftPressed: function(event) { root.selectRelative(-1); event.accepted = true }
                            Keys.onRightPressed: function(event) { root.selectRelative(1); event.accepted = true }

                            contentItem: Item {
                                ArtworkFrame {
                                    anchors.fill: parent
                                    source: root.showModuleArtwork ? carouselCard.tileArt : ""
                                    focalX: carouselCard.tileArtFocalX
                                    focalY: carouselCard.tileArtFocalY
                                }
                                StatusPill {
                                    visible: carouselCard.moduleUpdateAvailable
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.margins: Theme.spaceSm
                                    label: "UPDATE"
                                    tone: Theme.warning
                                }
                                Rectangle {
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                    height: 110
                                    gradient: Gradient {
                                        GradientStop { position: 0.0; color: "transparent" }
                                        GradientStop { position: 1.0; color: Qt.rgba(Theme.window.r, Theme.window.g, Theme.window.b, 0.96) }
                                    }
                                }
                                ColumnLayout {
                                    anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                    anchors.margins: Theme.spaceMd
                                    spacing: Theme.spaceXs
                                    Text { Layout.fillWidth: true; text: carouselCard.title; color: Theme.text; font.pixelSize: Theme.typeBody; font.weight: Font.DemiBold; wrapMode: Text.WordWrap; maximumLineCount: 2; elide: Text.ElideRight }
                                    Text { Layout.fillWidth: true; text: carouselCard.status; color: carouselCard.ready ? Theme.success : Theme.textMuted; font.pixelSize: Theme.typeCaption; elide: Text.ElideRight }
                                }
                            }
                            background: Rectangle {
                                radius: Theme.panelRadius
                                color: "transparent"
                                border.width: carouselCard.activeFocus ? Theme.focusWidth : (root.selectedGameIndex === carouselCard.index ? Theme.borderWidth * 2 : Theme.borderWidth)
                                border.color: carouselCard.activeFocus ? Theme.focusRing : (root.selectedGameIndex === carouselCard.index ? Theme.accent : Theme.border)
                            }
                            TapHandler {
                                acceptedButtons: Qt.RightButton
                                onTapped: function(eventPoint, button) {
                                    root.openGameContext(carouselCard.index, carouselCard, eventPoint.position.x, eventPoint.position.y)
                                }
                            }
                        }
                    }

                    GridView {
                        id: gridView
                        clip: true
                        model: gamesModel
                        cellWidth: width / Math.max(1, root.gridColumnCount)
                        cellHeight: Math.max(180, Math.min(260, cellWidth * 1.35))
                        currentIndex: root.selectedGameIndex
                        boundsBehavior: Flickable.StopAtBounds
                        cacheBuffer: Math.max(cellHeight * 2, 420)
                        reuseItems: true
                        highlightMoveDuration: Theme.reduceMotion ? 0 : 120
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff }
                        onCurrentIndexChanged: {
                            if (currentIndex >= 0 && currentIndex < gamesModel.count && root.selectedGameIndex !== currentIndex)
                                root.selectedGameIndex = currentIndex
                        }

                        delegate: Item {
                            id: gridCell
                            required property int index
                            required property string title
                            required property string status
                            required property bool ready
                            required property bool moduleUpdateAvailable
                            required property string tileArt
                            required property real tileArtFocalX
                            required property real tileArtFocalY
                            width: gridView.cellWidth
                            height: gridView.cellHeight

                            Button {
                                id: gridCard
                                anchors.fill: parent
                                anchors.margins: Theme.spaceXs
                                focusPolicy: Qt.StrongFocus
                                hoverEnabled: true
                                Accessible.name: gridCell.title
                                onClicked: { root.openGameDetails(gridCell.index); forceActiveFocus() }
                                onActiveFocusChanged: if (activeFocus) root.selectedGameIndex = gridCell.index

                                contentItem: Item {
                                    ArtworkFrame {
                                        anchors.fill: parent
                                        source: root.showModuleArtwork ? gridCell.tileArt : ""
                                        focalX: gridCell.tileArtFocalX
                                        focalY: gridCell.tileArtFocalY
                                    }
                                    StatusPill {
                                        visible: gridCell.moduleUpdateAvailable
                                        anchors.top: parent.top
                                        anchors.right: parent.right
                                        anchors.margins: Theme.spaceXs
                                        label: "UPDATE"
                                        tone: Theme.warning
                                    }
                                    Rectangle {
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                        height: Math.min(90, parent.height * 0.38)
                                        gradient: Gradient {
                                            GradientStop { position: 0.0; color: "transparent" }
                                            GradientStop { position: 1.0; color: Qt.rgba(Theme.window.r, Theme.window.g, Theme.window.b, 0.96) }
                                        }
                                    }
                                    ColumnLayout {
                                        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
                                        anchors.margins: Theme.spaceSm
                                        spacing: 2
                                        Text { Layout.fillWidth: true; text: gridCell.title; color: Theme.text; font.pixelSize: Theme.typeCaption; font.weight: Font.DemiBold; maximumLineCount: 2; elide: Text.ElideRight; wrapMode: Text.WordWrap }
                                        Text { Layout.fillWidth: true; text: gridCell.status; color: gridCell.ready ? Theme.success : Theme.textMuted; font.pixelSize: Math.max(10, Theme.typeCaption - 1); elide: Text.ElideRight }
                                    }
                                }
                                background: Rectangle {
                                    radius: Theme.panelRadius
                                    color: "transparent"
                                    border.width: gridCard.activeFocus ? Theme.focusWidth : (root.selectedGameIndex === gridCell.index ? Theme.borderWidth * 2 : Theme.borderWidth)
                                    border.color: gridCard.activeFocus ? Theme.focusRing : (root.selectedGameIndex === gridCell.index ? Theme.accent : Theme.border)
                                }
                                TapHandler {
                                    acceptedButtons: Qt.RightButton
                                    onTapped: function(eventPoint, button) {
                                        root.openGameContext(gridCell.index, gridCard, eventPoint.position.x, eventPoint.position.y)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        StackLayout {
            visible: root.detailOpen && root.hasGames && root.selectedGameIndex >= 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: 1

            EmptyState {
                glyph: "X"
                title: root.effectiveSearchText().length > 0 && root.selectedGameIndex < 0 ? "No matching games" : "Select a game"
                description: root.effectiveSearchText().length > 0 && root.selectedGameIndex < 0
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
                ScrollBar.vertical.policy: ScrollBar.AlwaysOff

                readonly property bool wide: availableWidth >= 1120 && Theme.textScale <= 1.30

                ColumnLayout {
                    // Keep a real right gutter inside the ScrollView. Without it,
                    // the DLC panel and its status text can sit beneath the native
                    // scrollbar / application edge on Windows.
                    width: Math.max(0, detailScroll.availableWidth - Theme.spaceLg)
                    spacing: Theme.spaceMd

                    RowLayout {
                        Layout.fillWidth: true
                        XButton {
                            text: "←  Back to Library"
                            variant: "ghost"
                            onClicked: root.closeGameDetails()
                        }
                        Item { Layout.fillWidth: true }
                    }

                    ArtworkFrame {
                        Layout.fillWidth: true
                        Layout.preferredHeight: detailScroll.wide ? 270 : 210
                        source: root.showModuleArtwork ? root.selectedGame().heroArt : ""
                        focalX: root.selectedGame().heroArtFocalX !== undefined ? root.selectedGame().heroArtFocalX : 0.5
                        focalY: root.selectedGame().heroArtFocalY !== undefined ? root.selectedGame().heroArtFocalY : 0.5
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

                        Row {
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            anchors.margins: Theme.spaceMd
                            spacing: Theme.spaceSm

                            StatusPill {
                                label: root.selectedGame().ready ? "Ready to Play" : root.selectedGame().status
                                tone: root.selectedGame().ready ? Theme.success : Theme.warning
                            }

                            XIconButton {
                                glyph: root.isFavorite(root.selectedGame().gameId) ? "★" : "☆"
                                tooltip: (root.isFavorite(root.selectedGame().gameId) ? "Remove from favorites" : "Add to favorites") + " (F)"
                                variant: "filled"
                                onClicked: root.toggleFavorite(root.selectedGame().gameId)
                            }
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

                                Flow {
                                    Layout.fillWidth: true
                                    spacing: Theme.spaceXs
                                    StatusPill {
                                        visible: Number(root.selectedGame().totalPlayTimeMs || 0) > 0
                                        label: "Playtime  " + root.formatDuration(root.selectedGame().totalPlayTimeMs)
                                        tone: Theme.textMuted
                                    }
                                    StatusPill {
                                        visible: String(root.selectedGame().lastPlayed || "").length > 0
                                            && String(root.selectedGame().lastPlayed || "") !== "Not launched"
                                        label: "Last played  " + String(root.selectedGame().lastPlayed)
                                        tone: Theme.textMuted
                                    }
                                    StatusPill {
                                        visible: dlcModel.count > 0
                                        label: dlcModel.count + (dlcModel.count === 1 ? " add-on" : " add-ons")
                                        tone: Theme.textMuted
                                    }
                                    StatusPill {
                                        visible: Boolean(root.selectedGame().moduleUpdateAvailable)
                                        label: "Module update available"
                                        tone: Theme.warning
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
                                            text: {
                                                if (root.selectedSessionState() === "stopping")
                                                    return "The launcher is waiting for the current session to stop cleanly."
                                                // First-launch automatic preparation (docs/development/GAME_PREPARATION.md)
                                                // reports real phase/progress text here while it builds a
                                                // native module; every other state keeps the generic message.
                                                var message = String(root.currentSession.progressMessage || "")
                                                if (root.selectedSessionState() === "preparing" && message.length > 0) {
                                                    var percent = root.currentSession.progressPercent
                                                    return (percent >= 0 ? ("(" + percent + "%) ") : "") + message
                                                }
                                                return "Xenon is progressing through the launcher-side session pipeline. You can cancel before execution begins."
                                            }
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
                                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOff; interactive: true }

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
        id: deleteManagedFilesConfirm
        title: "Permanently delete managed files for “" + root.pendingDeleteManagedGameTitle + "”?"
        message: "This permanently deletes Xenon's own managed copy of this game's files (media moved into the library, installed DLC, prepared build cache). This cannot be undone. The library entry itself is not removed - use \"Remove From Library\" separately for that."
        confirmText: "Delete managed files"
        destructive: true
        onConfirmed: {
            launcherBridge.deleteManagedGameFiles(root.pendingDeleteManagedGameId)
            root.pendingDeleteManagedGameId = ""
            root.pendingDeleteManagedGameTitle = ""
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
        title: "Add game from disc image or loose XEX"
        fileMode: FileDialog.OpenFiles
        // Xbox 360 disc images (.iso/.xgd/.dvd) are the primary, expected
        // workflow - a legally-owned disc image needs no manual extraction.
        // A loose default.xex remains available for the developer workflow
        // (docs/development/GAME_PREPARATION.md "Developer loose-XEX workflow").
        nameFilters: ["Xbox 360 disc images and executables (*.iso *.xgd *.dvd *.xex)", "All files (*)"]
        onAccepted: {
            root.pendingGameImportFiles = selectedFiles
            importModeDialog.open()
        }
    }

    // Move vs. Keep (docs/development/GAME_PREPARATION.md "Managed game library"): a
    // multi-gigabyte disc image is never moved without this explicit choice.
    Popup {
        id: importModeDialog
        modal: true
        focus: true
        anchors.centerIn: Overlay.overlay
        width: Math.min(480, parent ? parent.width - 48 : 480)
        padding: 20

        ColumnLayout {
            width: parent.width
            spacing: 14

            Label {
                text: "Add to Xenon Library"
                font.bold: true
                font.pixelSize: 16
            }
            Label {
                text: "Should Xenon move the selected file(s) into the managed Xenon Launcher library, or leave them in their current location?"
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }
            Label {
                text: "Moving a large disc image can take a while and requires enough free space at the destination until the move completes."
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
                opacity: 0.75
                font.pixelSize: 12
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: 10
                Item { Layout.fillWidth: true }
                Button {
                    text: "Keep in current location"
                    onClicked: {
                        importModeDialog.close()
                        if (launcherBridge.importGameContent(root.pendingGameImportFiles, false))
                            root.populateBackendLibrary()
                        root.pendingGameImportFiles = []
                    }
                }
                Button {
                    text: "Move into Xenon Library"
                    highlighted: true
                    onClicked: {
                        importModeDialog.close()
                        if (launcherBridge.importGameContent(root.pendingGameImportFiles, true))
                            root.populateBackendLibrary()
                        root.pendingGameImportFiles = []
                    }
                }
            }
        }
    }

    ModuleSettingsDialog {
        id: moduleSettingsDialog
        onSettingEdited: function(settingId, value) {
            launcherBridge.setModuleSetting(root.selectedGame().moduleId || "", settingId, value)
        }
    }
}
