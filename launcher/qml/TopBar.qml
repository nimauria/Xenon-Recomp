import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root

    property alias searchText: searchField.text
    property bool maximized: false
    property bool compact: false
    property int currentPage: 0

    signal minimizeRequested()
    signal maximizeRequested()
    signal closeRequested()

    implicitHeight: Math.max(compact ? 60 : 68, Theme.controlHeight + Theme.spaceLg + Theme.spaceSm)
    color: Theme.header
    border.width: Theme.borderWidth
    border.color: Theme.divider

    readonly property var searchPlaceholders: [
        "Search your library…",
        "Search installed modules…",
        "Search profiles…",
        "Find settings…"
    ]

    MouseArea {
        anchors.fill: parent
        z: 0
        acceptedButtons: Qt.LeftButton
        onPressed: {
            if (root.Window.window)
                root.Window.window.startSystemMove()
        }
        onDoubleClicked: root.maximizeRequested()
    }

    Shortcut {
        sequence: "Ctrl+K"
        onActivated: searchField.forceActiveFocus()
    }
    Shortcut {
        sequence: "Ctrl+F"
        onActivated: {
            if (root.currentPage === 3)
                searchField.forceActiveFocus()
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceLg
        anchors.rightMargin: 0
        spacing: Theme.spaceLg
        z: 2

        XenonBrand {
            // The full lockup contains intentionally tiny tagline text which is
            // not suitable for launcher chrome. Render the clean mark+wordmark
            // here and keep the tagline as accessible live text elsewhere.
            Layout.preferredWidth: root.compact ? 142 : 188
            Layout.preferredHeight: root.compact ? 34 : 40
            asset: "lockup"
            brandColor: Theme.accent
        }

        XTextField {
            id: searchField
            automationId: "global-search"
            Layout.fillWidth: true
            Layout.maximumWidth: 680
            Layout.preferredHeight: Theme.controlHeight
            placeholderText: root.searchPlaceholders[Math.max(0, Math.min(root.currentPage, root.searchPlaceholders.length - 1))]
            accessibleName: "Search current page"
            accessibleDescription: placeholderText
            Keys.onEscapePressed: clear()
        }

        Item { Layout.fillWidth: true }

        XIconButton {
            id: helpButton
            iconName: "help"
            tooltip: "Help and keyboard shortcuts"
            variant: "ghost"
            onClicked: helpPopup.open()

            HelpPopover {
                id: helpPopup
                x: helpButton.width - width
                y: helpButton.height + Theme.spaceXs
            }
        }

        ProfileMenu {
            Layout.preferredWidth: root.compact ? 176 : 210
            Layout.preferredHeight: Theme.controlHeight
        }

        Rectangle {
            Layout.preferredWidth: 1
            Layout.preferredHeight: 28
            color: Theme.divider
        }

        XWindowButton {
            automationId: "window-minimize"
            kind: "minimize"
            Layout.fillHeight: true
            onClicked: root.minimizeRequested()
        }

        XWindowButton {
            automationId: "window-maximize"
            kind: "maximize"
            maximized: root.maximized
            Layout.fillHeight: true
            onClicked: root.maximizeRequested()
        }

        XWindowButton {
            automationId: "window-close"
            kind: "close"
            Layout.fillHeight: true
            onClicked: root.closeRequested()
        }
    }
}
