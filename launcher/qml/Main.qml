import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    property var initialWindowState: launcherBridge.windowState()
    visible: true
    width: Number(initialWindowState.width || 1600)
    height: Number(initialWindowState.height || 900)
    minimumWidth: 1100
    minimumHeight: 700
    title: launcherBridge.safeMode ? "Xenon Launcher — Safe Mode" : "Xenon Launcher"
    color: Theme.window
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowSystemMenuHint | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint | Qt.WindowCloseButtonHint

    property int currentPage: 0
    // Page 9 (Developer) only exists in the pageForward/pageBack cycle order
    // while Developer Mode is actually on, matching it not existing in the
    // sidebar either.
    readonly property int pageCount: developerModeEnabled ? 10 : 9
    property alias globalSearchText: topBar.searchText
    property bool compactLayout: launcherBridge.safeMode ? false : settingBool("general/compact", false)
    property string sidebarMode: launcherBridge.safeMode ? "Expanded" : launcherBridge.stringSetting("general/sidebarMode", "Auto")
    property bool themeBackdropEnabled: launcherBridge.safeMode ? false : settingBool("appearance/themeBackdrop", true)
    property real backdropIntensity: launcherBridge.numberSetting("appearance/backdropIntensity", 0.72)
    property int backdropSettingsRevision: 0
    readonly property string backdropVariant: {
        var revision = backdropSettingsRevision
        return launcherBridge.themeBackgroundVariant(Theme.effectiveThemeId)
    }
    readonly property string backdropSource: {
        var revision = backdropSettingsRevision
        return launcherBridge.themeBackgroundAsset(Theme.effectiveThemeId, root.backdropVariant)
    }

    readonly property bool layoutCompact: compactLayout
        || width < (1240 * Math.min(Theme.textScale, 1.35))
    readonly property bool sidebarCompact: compactLayout || sidebarMode === "Compact"
        || (sidebarMode === "Auto" && width < (1320 * Math.min(Theme.textScale, 1.30)))

    property bool libraryLoaded: false
    property bool modulesLoaded: false
    property bool profilesLoaded: false
    property bool settingsLoaded: false
    property bool homeLoaded: false
    property bool downloadsLoaded: false
    property bool capturesLoaded: false
    property bool networkLoaded: false
    property bool supportLoaded: false
    property bool developerLoaded: false
    property bool developerModeEnabled: launcherBridge.safeMode ? false : settingBool("developer/modeEnabled", false)
    property bool launcherReadyMarked: false
    property int prewarmStep: 0
    property int pendingNavigationPage: -1
    property string pendingNavigationTarget: ""
    property string pendingNavigationSection: ""
    property bool windowTrackingReady: false
    property bool lastWindowMaximized: Boolean(initialWindowState.maximized)

    function scheduleWindowStateSave() {
        if (!windowTrackingReady || launcherBridge.safeMode) return
        windowStateSaveTimer.restart()
    }

    function persistWindowState() {
        if (!windowTrackingReady || launcherBridge.safeMode) return
        launcherBridge.saveWindowState(root.x, root.y, root.width, root.height, root.lastWindowMaximized)
    }

    function activateExistingWindow() {
        if (root.visibility === Window.Minimized) {
            if (root.lastWindowMaximized) root.showMaximized()
            else root.showNormal()
        }
        root.raise()
        root.requestActivate()
    }

    function settingBool(key, fallback) {
        return launcherBridge.boolSetting(key, fallback)
    }

    function applyThemeFromBackend() {
        Theme.setSystemAppearance(launcherBridge.systemDark, launcherBridge.systemHighContrast)
        var effectiveId = launcherBridge.safeMode ? "xenon-dark" : launcherBridge.effectiveThemeId()
        var selectedThemeId = launcherBridge.safeMode ? "xenon-dark" : launcherBridge.themeId
        var selectedAccentId = launcherBridge.safeMode ? "default" : launcherBridge.accentId
        Theme.applyAppearance(
            selectedThemeId,
            effectiveId,
            launcherBridge.themeDefinition(effectiveId),
            selectedAccentId,
            launcherBridge.accentDefinition(selectedAccentId))
        Theme.setCornerStyle(launcherBridge.safeMode ? "Rounded" : launcherBridge.cornerStyle)
        Theme.setAccessibility(
            launcherBridge.numberSetting("accessibility/textScale", 1.0),
            root.settingBool("accessibility/highContrast", false),
            root.settingBool("accessibility/enhancedFocus", false),
            root.settingBool("accessibility/reduceMotion", false))
        Theme.setAdvancedAppearance(
            launcherBridge.safeMode ? "Minimal" : launcherBridge.stringSetting("appearance/decorLevel", "Balanced"),
            launcherBridge.safeMode ? 1.0 : launcherBridge.numberSetting("appearance/panelOpacity", 0.94))
        if (launcherBridge.safeMode) root.themeBackdropEnabled = false
        root.backdropSettingsRevision += 1
    }

    function updateHandheldLayout() {
        Theme.handheld = root.width <= 1280
    }

    function toggleMaximize() {
        root.visibility = root.visibility === Window.Maximized
            ? Window.Windowed
            : Window.Maximized
        Qt.callLater(function() { root.requestActivate() })
    }

    // The single dispatch point every input method (keyboard Shortcut items
    // below, and xenon::input's FrontendInputRouter via
    // launcherBridge.frontendAction - gamepad D-pad/stick/buttons) funnels
    // through, so "what an action does" is defined exactly once regardless
    // of which device triggered it (Part 2/3).
    function handleFrontendAction(action) {
        // A modal that wants to interpret navigation actions itself (e.g.
        // AvatarCropEditor's controller support) has claimed input via
        // NavigationGuard - step aside rather than also reacting to the
        // same signal (Qt dispatches one signal to every connected slot).
        if (NavigationGuard.modalActive) return
        if (action === "quickCenter" || action === "menu") {
            topBar.toggleQuickCenter()
        } else if (action === "search") {
            topBar.focusSearch()
        } else if (action === "up" || action === "down") {
            var directionalPage = root.activePageItem()
            if (!directionalPage || typeof directionalPage.handleDirectionalNavigation !== "function"
                    || !directionalPage.handleDirectionalNavigation(action))
                root.moveFocusedControl(action === "down")
        } else if (action === "confirm") {
            root.activateFocusedControl()
        } else if (action === "pageBack") {
            root.currentPage = (root.currentPage + root.pageCount - 1) % root.pageCount
        } else if (action === "pageForward") {
            root.currentPage = (root.currentPage + 1) % root.pageCount
        } else if (action === "left" || action === "right") {
            var lateralPage = root.activePageItem()
            if (lateralPage && typeof lateralPage.handleDirectionalNavigation === "function")
                lateralPage.handleDirectionalNavigation(action)
        } else if (action === "cancel" || action === "back") {
            root.handleBackAction()
        } else if (action === "context") {
            root.openContextMenuForCurrentPage()
        } else if (action === "secondary") {
            root.triggerSecondaryActionForCurrentPage()
        } else if (action === "scrollUp" || action === "scrollDown") {
            root.movePageInFocusedList(action === "scrollDown")
        }
    }

    // Escape / Backspace / Alt+Left / gamepad B: close whatever is
    // topmost first (QuickCenter, then any open Popup - a Popup already
    // accepts Escape itself via CloseOnEscape before this ever runs, so
    // reaching here means none was open) - never jumps to a different
    // top-level page or resets the launcher (Part 9).
    function handleBackAction() {
        if (topBar.quickCenterVisible()) {
            topBar.closeQuickCenter()
            return
        }
        var page = root.activePageItem()
        if (page && typeof page.handleBackNavigation === "function" && page.handleBackNavigation())
            return
        // No further history to pop - a terminal state, not an error.
    }

    function loaderForPage(page) {
        if (page === 0) return libraryLoader
        if (page === 1) return modulesLoader
        if (page === 2) return profilesLoader
        if (page === 3) return settingsLoader
        if (page === 4) return homeLoader
        if (page === 5) return downloadsLoader
        if (page === 6) return capturesLoader
        if (page === 7) return networkLoader
        if (page === 8) return supportLoader
        if (page === 9) return developerLoader
        return homeLoader
    }

    function activePageItem() {
        var loader = root.loaderForPage(root.currentPage)
        return (loader && loader.status === Loader.Ready) ? loader.item : null
    }

    function focusCurrentPageSearch() {
        var page = root.activePageItem()
        if (page && typeof page.focusSearch === "function") {
            page.focusSearch()
            return
        }
        topBar.focusSearch()
    }

    // Shift+F10 / Menu key / gamepad Y: opens the same context menu
    // right-click and each page's visible overflow button already open -
    // one action model, three input paths (Part 17).
    function openContextMenuForCurrentPage() {
        var page = root.activePageItem()
        if (page && typeof page.openContextMenuForFocusedItem === "function")
            page.openContextMenuForFocusedItem()
    }

    function triggerSecondaryActionForCurrentPage() {
        var page = root.activePageItem()
        if (page && typeof page.triggerSecondaryAction === "function")
            page.triggerSecondaryAction()
    }

    function movePageInFocusedList(forward) {
        var page = root.activePageItem()
        if (page && typeof page.movePage === "function") {
            page.movePage(forward)
            return
        }
        // No page-level paging handler - fall back to moving focus by a
        // handful of steps so PageUp/PageDown/triggers still do *something*
        // reasonable on pages that have not opted into precise paging.
        for (var i = 0; i < 5; ++i) root.moveFocusedControl(forward)
    }

    function moveFocusedControl(forward) {
        var focused = root.activeFocusItem
        if (!focused) {
            root.forceActiveFocus()
            return
        }
        var next = focused.nextItemInFocusChain(forward)
        if (next)
            next.forceActiveFocus()
    }

    function activateFocusedControl() {
        var focused = root.activeFocusItem
        if (!focused)
            return
        if (typeof focused.click === "function")
            focused.click()
        else if (typeof focused.clicked === "function")
            focused.clicked()
        else if (typeof focused.trigger === "function")
            focused.trigger()
    }

    // Keep keyboard/controller focus inside the visible viewport. This is
    // intentionally generic: pages can use ScrollView, ListView, GridView or
    // Flickable without each implementing its own focus-scroll workaround.
    function ensureFocusVisible(item) {
        if (!item) return
        var node = item.parent
        while (node) {
            var hasVerticalScroll = typeof node.contentY !== "undefined"
                && typeof node.contentHeight !== "undefined" && typeof node.height !== "undefined"
            var hasHorizontalScroll = typeof node.contentX !== "undefined"
                && typeof node.contentWidth !== "undefined" && typeof node.width !== "undefined"
            if (hasVerticalScroll || hasHorizontalScroll) {
                if (!Boolean(node.dragging)) {
                    var mapped = item.mapToItem(node, 0, 0)
                    var margin = Theme.spaceMd
                    if (hasVerticalScroll && Number(node.contentHeight || 0) > Number(node.height || 0)) {
                        var nextY = Number(node.contentY || 0)
                        if (mapped.y < margin)
                            nextY += mapped.y - margin
                        else if (mapped.y + item.height > node.height - margin)
                            nextY += mapped.y + item.height - node.height + margin
                        var maxY = Math.max(0, Number(node.contentHeight || 0) - Number(node.height || 0))
                        node.contentY = Math.max(0, Math.min(maxY, nextY))
                    }
                    if (hasHorizontalScroll && Number(node.contentWidth || 0) > Number(node.width || 0)) {
                        var nextX = Number(node.contentX || 0)
                        if (mapped.x < margin)
                            nextX += mapped.x - margin
                        else if (mapped.x + item.width > node.width - margin)
                            nextX += mapped.x + item.width - node.width + margin
                        var maxX = Math.max(0, Number(node.contentWidth || 0) - Number(node.width || 0))
                        node.contentX = Math.max(0, Math.min(maxX, nextX))
                    }
                }
                // The nearest scrolling viewport is the one that owns this
                // control; outer page scrolling should not fight nested lists.
                return
            }
            node = node.parent
        }
    }

    function markPageLoaded(page) {
        if (page === 0) libraryLoaded = true
        else if (page === 1) modulesLoaded = true
        else if (page === 2) profilesLoaded = true
        else if (page === 3) settingsLoaded = true
        else if (page === 4) homeLoaded = true
        else if (page === 5) downloadsLoaded = true
        else if (page === 6) capturesLoaded = true
        else if (page === 7) networkLoaded = true
        else if (page === 8) supportLoaded = true
        else if (page === 9) developerLoaded = true
    }

    function pageName(page) {
        if (page === 1) return "Modules"
        if (page === 2) return "Profiles"
        if (page === 3) return "Settings"
        if (page === 4) return "Home"
        if (page === 5) return "Downloads"
        if (page === 6) return "Captures"
        if (page === 7) return "Network"
        if (page === 8) return "Support"
        if (page === 9) return "Developer"
        return "Library"
    }

    function clearPendingNavigation() {
        pendingNavigationPage = -1
        pendingNavigationTarget = ""
        pendingNavigationSection = ""
    }

    function requestCommandNavigation(page, targetId, sectionId) {
        pendingNavigationPage = Number(page)
        pendingNavigationTarget = String(targetId || "")
        pendingNavigationSection = String(sectionId || "")
        currentPage = pendingNavigationPage
        markPageLoaded(pendingNavigationPage)
        Qt.callLater(root.applyPendingNavigation)
    }

    function applyPendingNavigation() {
        var page = pendingNavigationPage
        if (page < 0) return

        var loader = root.loaderForPage(page)
        if (!loader || loader.status !== Loader.Ready || !loader.item) return

        var handled = true
        if (page === 0 && pendingNavigationTarget.length > 0) {
            handled = loader.item.selectGameById(pendingNavigationTarget)
        } else if (page === 1) {
            if (pendingNavigationSection === "catalog")
                loader.item.openCatalog()
            else if (pendingNavigationTarget.length > 0)
                handled = loader.item.selectModuleById(pendingNavigationTarget)
        } else if (page === 2) {
            if (pendingNavigationSection === "create")
                loader.item.openCreateProfile()
            else if (pendingNavigationTarget.length > 0)
                handled = loader.item.selectProfileById(pendingNavigationTarget)
        } else if (page === 3 && pendingNavigationTarget.length > 0) {
            handled = loader.item.selectCategoryById(pendingNavigationTarget)
        }

        root.clearPendingNavigation()
        if (!handled)
            launcherBridge.notify("Command palette", "The selected item is no longer available.")
    }

    function markLauncherReadyIfCurrent(page, status) {
        if (root.launcherReadyMarked || root.currentPage !== page || status !== Loader.Ready)
            return
        root.launcherReadyMarked = true
        launcherBridge.markLauncherReady()
        var recovery = launcherBridge.recoveryState
        if (Boolean(recovery.recoveryPromptPending)) recoveryDialog.open()
    }

    Component.onCompleted: {
        ProfileStore.reset(launcherBridge.profileName, launcherBridge.testMode)
        root.updateHandheldLayout()
        root.applyThemeFromBackend()

        currentPage = launcherBridge.initialPage()
        launcherBridge.setCommunityPage(root.pageName(currentPage))
        // Delay the first heavy page until the root window exists. The process
        // is only marked interactive after that asynchronous page reaches
        // Loader.Ready, so a crash while constructing the initial page is still
        // classified as a startup failure on the next run.
        Qt.callLater(function() { root.markPageLoaded(root.currentPage) })

        if (Boolean(root.initialWindowState.hasPosition)) {
            root.x = Number(root.initialWindowState.x || 0)
            root.y = Number(root.initialWindowState.y || 0)
        }
        root.windowTrackingReady = true
        if (Boolean(root.initialWindowState.maximized))
            root.showMaximized()
        if (Boolean(root.initialWindowState.startMinimized))
            Qt.callLater(function() { root.showMinimized() })
    }

    onActiveFocusItemChanged: {
        var focused = root.activeFocusItem
        if (focused) Qt.callLater(function() { root.ensureFocusVisible(focused) })
    }

    onXChanged: scheduleWindowStateSave()
    onYChanged: scheduleWindowStateSave()
    onWidthChanged: {
        scheduleWindowStateSave()
        root.updateHandheldLayout()
    }
    onHeightChanged: scheduleWindowStateSave()
    onVisibilityChanged: {
        if (visibility === Window.Maximized) root.lastWindowMaximized = true
        else if (visibility === Window.Windowed) root.lastWindowMaximized = false
        scheduleWindowStateSave()
    }
    onClosing: function(close) { root.persistWindowState() }

    Timer {
        id: windowStateSaveTimer
        interval: 450
        repeat: false
        onTriggered: root.persistWindowState()
    }

    // Once the first page is interactive, construct a few frequently-used
    // pages asynchronously during idle time. This keeps startup lean while
    // making the first Home/Downloads/Modules switch feel immediate.
    Timer {
        id: pagePrewarmTimer
        interval: 1300
        repeat: true
        running: root.launcherReadyMarked && !launcherBridge.safeMode && root.prewarmStep < 3
        onTriggered: {
            var pages = [4, 5, 1]
            root.markPageLoaded(pages[root.prewarmStep])
            root.prewarmStep += 1
        }
    }

    onCurrentPageChanged: {
        markPageLoaded(currentPage)
        if (topBar)
            topBar.searchText = ""
        if (!launcherBridge.safeMode) launcherBridge.rememberPage(currentPage)
        launcherBridge.setCommunityPage(root.pageName(currentPage))
    }

    Shortcut { sequence: "Ctrl+H"; onActivated: root.currentPage = 4 }
    Shortcut { sequence: "Ctrl+1"; onActivated: root.currentPage = 0 }
    Shortcut { sequence: "Ctrl+2"; onActivated: root.currentPage = 1 }
    Shortcut { sequence: "Ctrl+3"; onActivated: root.currentPage = 2 }
    Shortcut { sequence: "Ctrl+,"; onActivated: root.currentPage = 3 }
    Shortcut { sequence: "Ctrl+J"; onActivated: topBar.toggleQuickCenter() }
    Shortcut { sequence: "Ctrl+F"; onActivated: root.focusCurrentPageSearch() }

    // Keyboard-only operation (Part 4). These reach the exact same
    // handleFrontendAction()/handleBackAction() a gamepad also drives - see
    // that function's own comment. Qt gives a focused TextField/TextArea
    // first refusal on every one of these keys (arrow-key cursor movement,
    // Backspace character deletion) before a Shortcut ever sees them, so
    // none of this steals editing keys from text inputs.
    Shortcut { sequence: "Up"; onActivated: root.handleFrontendAction("up") }
    Shortcut { sequence: "Down"; onActivated: root.handleFrontendAction("down") }
    Shortcut { sequence: "Left"; onActivated: root.handleFrontendAction("left") }
    Shortcut { sequence: "Right"; onActivated: root.handleFrontendAction("right") }
    Shortcut { sequence: "PageUp"; onActivated: root.handleFrontendAction("scrollUp") }
    Shortcut { sequence: "PageDown"; onActivated: root.handleFrontendAction("scrollDown") }
    Shortcut { sequence: "Escape"; onActivated: root.handleFrontendAction("back") }
    Shortcut { sequence: "Backspace"; onActivated: root.handleFrontendAction("back") }
    Shortcut { sequence: "Alt+Left"; onActivated: root.handleFrontendAction("back") }
    Shortcut { sequence: "Shift+F10"; onActivated: root.handleFrontendAction("context") }
    Shortcut { sequence: "Menu"; onActivated: root.handleFrontendAction("context") }

    Connections {
        target: launcherBridge
        function onThemeIdChanged() { root.applyThemeFromBackend() }
        function onAccentIdChanged() { root.applyThemeFromBackend() }
        function onCustomAccentColorChanged() { root.applyThemeFromBackend() }
        function onCornerStyleChanged() { root.applyThemeFromBackend() }
        function onSystemAppearanceChanged() { root.applyThemeFromBackend() }
        function onNotificationRequested(title, message) { toast.show(title, message) }
        function onFrontendAction(action) { root.handleFrontendAction(action) }
        function onNavigationRequested(pageIndex, targetId, sectionId) {
            root.requestCommandNavigation(pageIndex, targetId, sectionId)
        }
        function onWindowActivationRequested() { root.activateExistingWindow() }
        function onWindowMinimizeRequested() { root.showMinimized() }
        function onSettingChanged(key, value) {
            if (key === "general/compact" && !launcherBridge.safeMode)
                root.compactLayout = root.settingBool(key, false)
            else if (key === "general/sidebarMode" && !launcherBridge.safeMode)
                root.sidebarMode = launcherBridge.stringSetting(key, "Auto")
            else if (key === "appearance/themeBackdrop" && !launcherBridge.safeMode)
                root.themeBackdropEnabled = root.settingBool(key, true)
            else if (key === "appearance/backdropIntensity")
                root.backdropIntensity = launcherBridge.numberSetting(key, 0.72)
            else if (String(key).indexOf("appearance/backdropVariant/") === 0)
                root.backdropSettingsRevision += 1
            else if ((key === "appearance/decorLevel" || key === "appearance/panelOpacity") && !launcherBridge.safeMode)
                Theme.setAdvancedAppearance(
                    launcherBridge.stringSetting("appearance/decorLevel", "Balanced"),
                    launcherBridge.numberSetting("appearance/panelOpacity", 0.94))
            else if (key === "accessibility/textScale" || key === "accessibility/highContrast"
                     || key === "accessibility/enhancedFocus" || key === "accessibility/reduceMotion")
                Theme.setAccessibility(
                    launcherBridge.numberSetting("accessibility/textScale", 1.0),
                    root.settingBool("accessibility/highContrast", false),
                    root.settingBool("accessibility/enhancedFocus", false),
                    root.settingBool("accessibility/reduceMotion", false))
            else if (key === "developer/modeEnabled" && !launcherBridge.safeMode) {
                root.developerModeEnabled = root.settingBool(key, false)
                // The Developer page just lost its sidebar entry - it must
                // not remain "current" with no way back to it via navigation.
                if (!root.developerModeEnabled && root.currentPage === 9)
                    root.currentPage = 4
            }
        }
    }

    Component {
        id: homePageComponent
        HomePage { }
    }

    Component {
        id: libraryPageComponent
        LibraryPage {
            searchText: root.globalSearchText
            onRequestPage: function(index) { root.currentPage = index }
        }
    }

    Component {
        id: modulesPageComponent
        ModulesPage { searchText: root.globalSearchText }
    }

    Component {
        id: profilesPageComponent
        ProfilesPage { searchText: root.globalSearchText }
    }

    Component {
        id: settingsPageComponent
        SettingsPage { searchText: root.globalSearchText }
    }

    Component {
        id: downloadsPageComponent
        DownloadsPage {
            onRequestPage: function(index) { root.currentPage = index }
        }
    }

    Component {
        id: capturesPageComponent
        CapturesPage { searchText: root.globalSearchText }
    }

    Component {
        id: networkPageComponent
        NetworkPage { }
    }

    Component {
        id: supportPageComponent
        SupportPage { }
    }

    Component {
        id: developerPageComponent
        DeveloperPage { }
    }

    Component {
        id: safeModeBlockedPage
        EmptyState {
            glyph: "!"
            title: "Unavailable in Safe Mode"
            description: "This launcher area is intentionally not loaded in Safe Mode. Use Settings and diagnostics to recover, then restart Xenon normally."
            primaryText: "Open Settings"
            secondaryText: "Restart Normally"
            onPrimaryClicked: root.currentPage = 3
            onSecondaryClicked: launcherBridge.restartNormally()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TopBar {
            id: topBar
            Layout.fillWidth: true
            compact: root.layoutCompact
            currentPage: root.currentPage
            maximized: root.visibility === Window.Maximized
            onMinimizeRequested: root.showMinimized()
            onMaximizeRequested: root.toggleMaximize()
            onCloseRequested: root.close()
            onDownloadsRequested: root.currentPage = 5
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: safeModeRow.implicitHeight + Theme.spaceSm * 2
            visible: launcherBridge.safeMode
            color: Theme.surfaceRaised
            border.width: Theme.borderWidth
            border.color: Theme.warning

            RowLayout {
                id: safeModeRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: Theme.spaceLg
                anchors.rightMargin: Theme.spaceLg
                spacing: Theme.spaceSm

                StatusPill { label: "SAFE MODE"; tone: Theme.warning }
                Text {
                    Layout.fillWidth: true
                    text: "Production launcher state and runtime services are not loaded. Use Settings or diagnostics to recover, then restart normally."
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                    wrapMode: Text.WordWrap
                }
                XButton { text: "Recovery Folder"; onClicked: launcherBridge.openFolder(launcherBridge.recoveryDirectory()) }
                XButton { text: "Restart Normally"; variant: "primary"; onClicked: launcherBridge.restartNormally() }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Sidebar {
                id: sidebar
                Layout.fillHeight: true
                compact: root.sidebarCompact
                backdropEnabled: root.themeBackdropEnabled
                backdropIntensity: root.backdropIntensity
                backdropVariant: root.backdropVariant
                backdropSource: root.backdropSource
                currentIndex: root.currentPage
                developerModeEnabled: root.developerModeEnabled
                onPageRequested: function(index) { root.currentPage = index }
                onCompactToggleRequested: {
                    var next = root.sidebarCompact ? "Expanded" : "Compact"
                    launcherBridge.setSettingValue("general/sidebarMode", next)
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.window
                clip: true

                ThemeBackdrop {
                    anchors.fill: parent
                    visible: root.themeBackdropEnabled
                    intensity: root.backdropIntensity
                    variant: root.backdropVariant
                    source: root.backdropSource
                }

                StackLayout {
                    anchors.fill: parent
                    anchors.margins: root.layoutCompact ? Theme.spaceMd : Theme.pageMargin
                    currentIndex: root.currentPage

                    Loader {
                        id: libraryLoader
                        active: root.libraryLoaded
                        asynchronous: true
                        sourceComponent: launcherBridge.safeMode ? safeModeBlockedPage : libraryPageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(0, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }

                    Loader {
                        id: modulesLoader
                        active: root.modulesLoaded
                        asynchronous: true
                        sourceComponent: launcherBridge.safeMode ? safeModeBlockedPage : modulesPageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(1, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }

                    Loader {
                        id: profilesLoader
                        active: root.profilesLoaded
                        asynchronous: true
                        sourceComponent: launcherBridge.safeMode ? safeModeBlockedPage : profilesPageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(2, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }

                    Loader {
                        id: settingsLoader
                        active: root.settingsLoaded
                        asynchronous: true
                        sourceComponent: settingsPageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(3, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }

                    Loader {
                        id: homeLoader
                        active: root.homeLoaded
                        asynchronous: true
                        sourceComponent: launcherBridge.safeMode ? safeModeBlockedPage : homePageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(4, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }

                    Loader {
                        id: downloadsLoader
                        active: root.downloadsLoaded
                        asynchronous: true
                        sourceComponent: launcherBridge.safeMode ? safeModeBlockedPage : downloadsPageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(5, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }

                    Loader {
                        id: capturesLoader
                        active: root.capturesLoaded
                        asynchronous: true
                        sourceComponent: launcherBridge.safeMode ? safeModeBlockedPage : capturesPageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(6, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }

                    Loader {
                        id: networkLoader
                        active: root.networkLoaded
                        asynchronous: true
                        sourceComponent: launcherBridge.safeMode ? safeModeBlockedPage : networkPageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(7, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }

                    Loader {
                        id: supportLoader
                        active: root.supportLoaded
                        asynchronous: true
                        sourceComponent: launcherBridge.safeMode ? safeModeBlockedPage : supportPageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(8, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }

                    Loader {
                        id: developerLoader
                        active: root.developerLoaded && root.developerModeEnabled
                        asynchronous: true
                        sourceComponent: launcherBridge.safeMode ? safeModeBlockedPage : developerPageComponent
                        onStatusChanged: {
                            root.markLauncherReadyIfCurrent(9, status)
                            if (status === Loader.Ready) root.applyPendingNavigation()
                        }
                    }
                }
            }
        }

        FooterBar { Layout.fillWidth: true }
    }

    Toast { id: toast; parent: Overlay.overlay }
    RecoveryDialog { id: recoveryDialog; parent: Overlay.overlay }

    // Frameless-window resize zones. Window controls intentionally do not hold
    // keyboard focus after mouse clicks, avoiding a stuck focus rectangle when
    // maximising/restoring on Windows.
    MouseArea {
        anchors.left: parent.left; anchors.top: parent.top
        width: 6; height: 6; z: 1001
        enabled: root.visibility !== Window.Maximized
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.startSystemResize(Qt.LeftEdge | Qt.TopEdge)
    }
    MouseArea {
        anchors.right: parent.right; anchors.top: parent.top
        width: 6; height: 6; z: 1001
        enabled: root.visibility !== Window.Maximized
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.startSystemResize(Qt.RightEdge | Qt.TopEdge)
    }
    MouseArea {
        anchors.left: parent.left; anchors.bottom: parent.bottom
        width: 6; height: 6; z: 1001
        enabled: root.visibility !== Window.Maximized
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.startSystemResize(Qt.LeftEdge | Qt.BottomEdge)
    }
    MouseArea {
        anchors.right: parent.right; anchors.bottom: parent.bottom
        width: 6; height: 6; z: 1001
        enabled: root.visibility !== Window.Maximized
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.startSystemResize(Qt.RightEdge | Qt.BottomEdge)
    }
    MouseArea {
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
        width: 5; z: 1000; cursorShape: Qt.SizeHorCursor
        enabled: root.visibility !== Window.Maximized
        onPressed: root.startSystemResize(Qt.LeftEdge)
    }
    MouseArea {
        anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
        width: 5; z: 1000; cursorShape: Qt.SizeHorCursor
        enabled: root.visibility !== Window.Maximized
        onPressed: root.startSystemResize(Qt.RightEdge)
    }
    MouseArea {
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
        height: 5; z: 1000; cursorShape: Qt.SizeVerCursor
        enabled: root.visibility !== Window.Maximized
        onPressed: root.startSystemResize(Qt.TopEdge)
    }
    MouseArea {
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
        height: 5; z: 1000; cursorShape: Qt.SizeVerCursor
        enabled: root.visibility !== Window.Maximized
        onPressed: root.startSystemResize(Qt.BottomEdge)
    }
}
