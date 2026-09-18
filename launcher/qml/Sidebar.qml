import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property int currentIndex: 0
    property bool compact: false
    property bool backdropEnabled: true
    property real backdropIntensity: 0.72
    property string backdropVariant: "default"
    signal pageRequested(int index)
    signal compactToggleRequested()

    implicitWidth: compact ? Theme.sidebarCompactWidth : Theme.sidebarWidth
    color: Theme.sidebar
    border.width: Theme.borderWidth
    border.color: Theme.divider

    ThemeBackdrop {
        anchors.fill: parent
        visible: root.backdropEnabled
        intensity: root.backdropIntensity
        variant: root.backdropVariant
        subtle: true
    }

    readonly property var primaryEntries: [
        { title: "Library", icon: "library", page: 0 },
        { title: "Modules", icon: "modules", page: 1 },
        { title: "Profiles", icon: "profiles", page: 2 }
    ]

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.spaceMd
        anchors.bottomMargin: Theme.spaceMd
        anchors.leftMargin: root.compact ? Theme.spaceMd : Theme.spaceMd
        anchors.rightMargin: root.compact ? Theme.spaceMd : Theme.spaceMd
        spacing: Theme.spaceSm

        // Compact/expanded navigation is a first-class control and lives at
        // the top rather than being stranded below the version text.
        XIconButton {
            Layout.alignment: root.compact ? Qt.AlignHCenter : Qt.AlignRight
            iconName: root.compact ? "chevron-right" : "chevron-left"
            tooltip: root.compact ? "Expand navigation" : "Collapse navigation"
            variant: "ghost"
            onClicked: root.compactToggleRequested()
        }

        Repeater {
            model: root.primaryEntries
            delegate: XNavButton {
                required property var modelData
                Layout.fillWidth: true
                text: modelData.title
                iconName: modelData.icon
                compact: root.compact
                active: root.currentIndex === modelData.page
                automationId: "nav-" + modelData.title.toLowerCase()
                onClicked: root.pageRequested(modelData.page)
            }
        }

        Item { Layout.fillHeight: true }

        XNavButton {
            Layout.fillWidth: true
            text: "Settings"
            iconName: "settings"
            compact: root.compact
            active: root.currentIndex === 3
            automationId: "nav-settings"
            onClicked: root.pageRequested(3)
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Rectangle { width: 10; height: 10; radius: 5; color: Theme.success }
            ColumnLayout {
                visible: !root.compact
                Layout.fillWidth: true
                spacing: 1
                Text { text: "Xenon Ready"; color: Theme.success; font.pixelSize: Theme.typeCaption; font.weight: Font.DemiBold }
                Text {
                    text: launcherBridge.backendConnected ? "Runtime connected" : "Front-end ready"
                    color: Theme.textMuted
                    font.pixelSize: Theme.typeCaption
                    elide: Text.ElideRight
                }
            }
        }

        Text {
            visible: !root.compact
            text: "v" + launcherBridge.version
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
        }
    }
}
