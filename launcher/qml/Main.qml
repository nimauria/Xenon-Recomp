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
    property bool launcherReadyMarked: false
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

    function handleFrontendAction(action) {
        if (action === "quickCenter" || action === "menu") {
            topBar.toggleQuickCenter()
        } else if (action === "search") {
            topBar.focusSearch()
        } else if (action === "up" || action === "down") {
            root.moveFocusedControl(action === "down")
        } else if (action === "confirm") {
            root.activateFocusedControl()
        } else if (action === "pageBack" || action === "left") {
            root.currentPage = Math.max(0, root.currentPage - 1)
        } else if (action === "pageForward" || action === "right") {
            root.currentPage = Math.min(4, root.currentPage + 1)
        } else if (action === "cancel") {
            topBar.closeQuickCenter()
        }
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

    function markPageLoaded(page) {
        if (page === 0) libraryLoaded = true
        else if (page === 1) modulesLoaded = true
        else if (page === 2) profilesLoaded = true
        else if (page === 3) settingsLoaded = true
        else if (page === 4) homeLoaded = true
    }

    function pageName(page) {
        if (page === 1) return "Modules"
        if (page === 2) return "Profiles"
        if (page === 3) return "Settings"
        if (page === 4) return "Home"
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

        var loader = page === 0 ? libraryLoader
                   : page === 1 ? modulesLoader
                   : page === 2 ? profilesLoader
                   : page === 3 ? settingsLoader
                   : homeLoader
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
                        sourceComponent: Component { SettingsPage { searchText: root.globalSearchText } }
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
