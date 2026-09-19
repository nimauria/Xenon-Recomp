import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property int currentIndex: 0
    property bool compact: false
    property bool backdropEnabled: true
    property real backdropIntensity: 0.72
    property string backdropVariant: "default"
    property string backdropSource: ""
    signal pageRequested(int index)
    signal compactToggleRequested()

    implicitWidth: compact ? Theme.sidebarCompactWidth : Theme.sidebarWidth
    color: Theme.sidebar
    border.width: Theme.borderWidth
    border.color: Theme.divider

    readonly property string railSource: Theme.decorAsset("rail_vertical")


    ThemeBackdrop {
        anchors.fill: parent
        visible: root.backdropEnabled
        intensity: root.backdropIntensity
        variant: root.backdropVariant
        source: root.backdropSource
        subtle: true
    }

    Image {
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.rightMargin: 1
        width: 8
        height: Math.min(240, parent.height * 0.48)
        source: root.railSource
        visible: Theme.decorLevel !== "Minimal" && source.toString().length > 0
        fillMode: Image.Stretch
        opacity: root.compact ? 0.42 : 0.22
        smooth: true
    }

    readonly property var primaryEntries: [
        { title: "Home", icon: "home", page: 4 },
        { title: "Library", icon: "library", page: 0 },
        { title: "Modules", icon: "modules", page: 1 },
        { title: "Profiles", icon: "profiles", page: 2 }
    ]

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: Theme.spaceMd
        anchors.bottomMargin: Theme.spaceMd
        anchors.leftMargin: root.compact ? Theme.spaceSm : Theme.spaceMd
        anchors.rightMargin: root.compact ? Theme.spaceSm : Theme.spaceMd
        spacing: Theme.spaceSm

        XIconButton {
            Layout.preferredWidth: root.compact ? 40 : Theme.controlHeight
            Layout.preferredHeight: root.compact ? 40 : Theme.controlHeight
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
                Layout.fillWidth: !root.compact
                Layout.preferredWidth: root.compact ? 44 : -1
                Layout.alignment: root.compact ? Qt.AlignHCenter : Qt.AlignLeft
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
            Layout.fillWidth: !root.compact
            Layout.preferredWidth: root.compact ? 44 : -1
            Layout.alignment: root.compact ? Qt.AlignHCenter : Qt.AlignLeft
            text: "Settings"
            iconName: "settings"
            compact: root.compact
            active: root.currentIndex === 3
            automationId: "nav-settings"
            onClicked: root.pageRequested(3)
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        RowLayout {
            Layout.fillWidth: !root.compact
            Layout.alignment: root.compact ? Qt.AlignHCenter : Qt.AlignLeft
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
