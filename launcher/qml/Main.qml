import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    visible: true
    width: 1600
    height: 900
    minimumWidth: 1100
    minimumHeight: 700
    title: "Xenon Launcher"
    color: Theme.window
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.CustomizeWindowHint | Qt.WindowSystemMenuHint | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint | Qt.WindowCloseButtonHint

    property int currentPage: 0
    property alias globalSearchText: topBar.searchText
    property bool compactLayout: settingBool("general/compact", false)
    property string sidebarMode: launcherBridge.stringSetting("general/sidebarMode", "Auto")
    property bool themeBackdropEnabled: settingBool("appearance/themeBackdrop", true)
    property real backdropIntensity: launcherBridge.numberSetting("appearance/backdropIntensity", 0.72)
    property int backdropSettingsRevision: 0
    readonly property string backdropVariant: {
        var revision = backdropSettingsRevision
        return launcherBridge.stringSetting(
            "appearance/backdropVariant/" + Theme.effectiveThemeId, "default")
    }

    readonly property bool layoutCompact: compactLayout
        || width < (1240 * Math.min(Theme.textScale, 1.35))
    readonly property bool sidebarCompact: compactLayout || sidebarMode === "Compact"
        || (sidebarMode === "Auto" && width < (1320 * Math.min(Theme.textScale, 1.30)))

    property bool libraryLoaded: true
    property bool modulesLoaded: false
    property bool profilesLoaded: false
    property bool settingsLoaded: false

    function settingBool(key, fallback) {
        return launcherBridge.boolSetting(key, fallback)
    }

    function toggleMaximize() {
        if (root.visibility === Window.Maximized)
            root.showNormal()
        else
            root.showMaximized()
        Qt.callLater(function() { root.requestActivate() })
    }

    function pageIndexFromName(name) {
        if (name === "Modules") return 1
        if (name === "Profiles") return 2
        if (name === "Settings") return 3
        return 0
    }

    function markPageLoaded(page) {
        if (page === 0) libraryLoaded = true
        else if (page === 1) modulesLoaded = true
        else if (page === 2) profilesLoaded = true
        else if (page === 3) settingsLoaded = true
    }

    Component.onCompleted: {
        ProfileStore.reset(launcherBridge.profileName, launcherBridge.testMode)
        Theme.setSystemAppearance(launcherBridge.systemDark, launcherBridge.systemHighContrast)
        Theme.setTheme(launcherBridge.themeId)
        Theme.setAccent(launcherBridge.accentId)
        Theme.setCornerStyle(launcherBridge.cornerStyle)
        Theme.setAccessibility(
            launcherBridge.numberSetting("accessibility/textScale", 1.0),
            settingBool("accessibility/highContrast", false))

        var remember = settingBool("general/rememberPage", true)
        if (remember) {
            var remembered = launcherBridge.rememberedPage()
            currentPage = Math.max(0, Math.min(remembered, 3))
        } else {
            currentPage = pageIndexFromName(launcherBridge.stringSetting("general/startupPage", "Library"))
        }
        markPageLoaded(currentPage)
    }

    onCurrentPageChanged: {
        markPageLoaded(currentPage)
        if (topBar)
            topBar.searchText = ""
        if (settingBool("general/rememberPage", true))
            launcherBridge.rememberPage(currentPage)
    }

    Shortcut { sequence: "Ctrl+1"; onActivated: root.currentPage = 0 }
    Shortcut { sequence: "Ctrl+2"; onActivated: root.currentPage = 1 }
    Shortcut { sequence: "Ctrl+3"; onActivated: root.currentPage = 2 }
    Shortcut { sequence: "Ctrl+,"; onActivated: root.currentPage = 3 }

    Connections {
        target: launcherBridge
        function onThemeIdChanged() { Theme.setTheme(launcherBridge.themeId) }
        function onAccentIdChanged() { Theme.setAccent(launcherBridge.accentId) }
        function onCornerStyleChanged() { Theme.setCornerStyle(launcherBridge.cornerStyle) }
        function onSystemAppearanceChanged() {
            Theme.setSystemAppearance(launcherBridge.systemDark, launcherBridge.systemHighContrast)
        }
        function onNotificationRequested(title, message) { toast.show(title, message) }
        function onSettingChanged(key, value) {
            if (key === "general/compact")
                root.compactLayout = root.settingBool(key, false)
            else if (key === "general/sidebarMode")
                root.sidebarMode = String(value)
            else if (key === "appearance/themeBackdrop")
                root.themeBackdropEnabled = root.settingBool(key, true)
            else if (key === "appearance/backdropIntensity")
                root.backdropIntensity = launcherBridge.numberSetting(key, 0.72)
            else if (String(key).indexOf("appearance/backdropVariant/") === 0)
                root.backdropSettingsRevision += 1
            else if (key === "accessibility/textScale" || key === "accessibility/highContrast")
                Theme.setAccessibility(
                    launcherBridge.numberSetting("accessibility/textScale", 1.0),
                    root.settingBool("accessibility/highContrast", false))
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
                }

                StackLayout {
                    anchors.fill: parent
                    anchors.margins: root.layoutCompact ? Theme.spaceMd : Theme.pageMargin
                    currentIndex: root.currentPage

                    Loader {
                        active: root.libraryLoaded
                        asynchronous: false
                        sourceComponent: Component {
                            LibraryPage {
                                searchText: root.globalSearchText
                                onRequestPage: function(index) { root.currentPage = index }
                            }
                        }
                    }

                    Loader {
                        active: root.modulesLoaded
                        asynchronous: true
                        sourceComponent: Component { ModulesPage { searchText: root.globalSearchText } }
                    }

                    Loader {
                        active: root.profilesLoaded
                        asynchronous: true
                        sourceComponent: Component { ProfilesPage { searchText: root.globalSearchText } }
                    }

                    Loader {
                        active: root.settingsLoaded
                        asynchronous: true
                        sourceComponent: Component { SettingsPage { searchText: root.globalSearchText } }
                    }
                }
            }
        }

        FooterBar { Layout.fillWidth: true }
    }

    Toast { id: toast; parent: Overlay.overlay }

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
