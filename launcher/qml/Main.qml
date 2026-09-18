import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root

    visible: true
    width: 1600
    height: 900
    minimumWidth: 1180
    minimumHeight: 720
    title: "Xenon Launcher"
    color: Theme.window
    flags: Qt.Window | Qt.FramelessWindowHint

    property int currentPage: 0
    property alias globalSearchText: topBar.searchText

    Component.onCompleted: {
        Theme.setTheme(launcherBridge.themeId)
        var remembered = launcherBridge.rememberedPage()
        currentPage = Math.max(0, Math.min(remembered, 3))
    }

    onCurrentPageChanged: launcherBridge.rememberPage(currentPage)

    Connections {
        target: launcherBridge
        function onThemeIdChanged() { Theme.setTheme(launcherBridge.themeId) }
        function onNotificationRequested(title, message) { toast.show(title, message) }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TopBar {
            id: topBar
            Layout.fillWidth: true
            maximized: root.visibility === Window.Maximized
            onMinimizeRequested: root.showMinimized()
            onMaximizeRequested: {
                if (root.visibility === Window.Maximized)
                    root.showNormal()
                else
                    root.showMaximized()
            }
            onCloseRequested: root.close()
            onProfilesRequested: root.currentPage = 2
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Sidebar {
                id: sidebar
                Layout.fillHeight: true
                currentIndex: root.currentPage
                onPageRequested: function(index) { root.currentPage = index }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.window

                StackLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    currentIndex: root.currentPage

                    LibraryPage {
                        searchText: root.globalSearchText
                        onRequestPage: function(index) { root.currentPage = index }
                    }

                    ModulesPage {
                        searchText: root.globalSearchText
                    }

                    ProfilesPage { }

                    SettingsPage { }
                }
            }
        }

        FooterBar {
            Layout.fillWidth: true
        }
    }

    Toast {
        id: toast
        parent: Overlay.overlay
    }

    // Frameless-window resize zones. They are intentionally narrow so they do
    // not interfere with normal launcher controls.
    MouseArea {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 5
        z: 1000
        cursorShape: Qt.SizeHorCursor
        onPressed: root.startSystemResize(Qt.LeftEdge)
    }
    MouseArea {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 5
        z: 1000
        cursorShape: Qt.SizeHorCursor
        onPressed: root.startSystemResize(Qt.RightEdge)
    }
    MouseArea {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 5
        z: 1000
        cursorShape: Qt.SizeVerCursor
        onPressed: root.startSystemResize(Qt.TopEdge)
    }
    MouseArea {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 5
        z: 1000
        cursorShape: Qt.SizeVerCursor
        onPressed: root.startSystemResize(Qt.BottomEdge)
    }
}
