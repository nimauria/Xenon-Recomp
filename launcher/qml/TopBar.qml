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

    readonly property int chromeHeight: 36
    readonly property int toolbarHeight: Math.max(60, Theme.controlHeight + 16)

    implicitHeight: chromeHeight + toolbarHeight
    color: Theme.header
    border.width: Theme.borderWidth
    border.color: Theme.divider

    readonly property var searchPlaceholders: [
        "Search your library…",
        "Search installed modules…",
        "Search profiles…",
        "Find settings…"
    ]

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

    Item {
        id: chromeRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.chromeHeight

        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.spaceLg
            anchors.verticalCenter: parent.verticalCenter
            text: "Xenon Launcher"
            color: Theme.textMuted
            font.pixelSize: Math.max(11, Theme.typeCaption * 0.86)
            font.weight: Font.Medium
        }

        RowLayout {
            id: windowControls
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            spacing: 0
            z: 4

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

        MouseArea {
            id: titleDragArea
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: windowControls.left
            acceptedButtons: Qt.LeftButton
            z: 1
            onPressed: {
                if (root.Window.window)
                    root.Window.window.startSystemMove()
            }
            onDoubleClicked: root.maximizeRequested()
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: chromeRow.bottom
        height: 1
        color: Theme.divider
    }

    RowLayout {
        id: toolbarRow
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: chromeRow.bottom
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.spaceXl
        anchors.rightMargin: Theme.spaceXl
        spacing: Theme.spaceMd

        XenonBrand {
            Layout.preferredWidth: root.compact ? 132 : 168
            Layout.preferredHeight: root.compact ? 30 : 34
            asset: "lockup"
            brandColor: Theme.accent
        }

        XTextField {
            id: searchField
            automationId: "global-search"
            Layout.fillWidth: true
            Layout.maximumWidth: 660
            Layout.preferredHeight: Theme.controlHeight
            placeholderText: root.searchPlaceholders[Math.max(0, Math.min(root.currentPage, root.searchPlaceholders.length - 1))]
            accessibleName: "Search current page"
            accessibleDescription: placeholderText
            Keys.onEscapePressed: clear()
        }

        Item { Layout.fillWidth: true }

        XIconButton {
            id: helpButton
            Layout.preferredWidth: Theme.controlHeight
            Layout.preferredHeight: Theme.controlHeight
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
            Layout.preferredWidth: root.compact ? 166 : 196
            Layout.preferredHeight: Theme.controlHeight
        }
    }

    Image {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.spaceLg
        width: Math.min(300, parent.width * 0.28)
        height: 18
        source: Theme.effectiveThemeId === "industrial"
            ? "qrc:/theme-art/decor/amber/divider_notch_amber.svg"
            : Theme.effectiveThemeId === "carbon"
              ? "qrc:/theme-art/decor/green/divider_notch_green.svg"
              : Theme.effectiveThemeId === "xenon-dark"
                ? "qrc:/theme-art/decor/blue/divider_notch_blue.svg" : ""
        visible: source.toString().length > 0
        fillMode: Image.PreserveAspectFit
        opacity: 0.36
        smooth: true
    }
}
