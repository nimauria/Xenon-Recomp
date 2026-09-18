import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property int categoryIndex: 0

    readonly property var categories: [
        "General",
        "Appearance",
        "Paths",
        "Runtime",
        "Updates",
        "About"
    ]

    readonly property var themeChoices: [
        { id: "xenon-cyan", name: "Xenon Cyan", description: "Dark navy with cyan accents." },
        { id: "xenon-green", name: "Xenon Green", description: "Dark graphite with green accents." },
        { id: "industrial-amber", name: "Industrial Amber", description: "Gunmetal panels with amber accents." },
        { id: "light", name: "Light", description: "Clean white and pale-grey interface." }
    ]

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        ColumnLayout {
            spacing: 2
            Text {
                text: "Settings"
                color: Theme.text
                font.pixelSize: 30
                font.weight: Font.DemiBold
            }
            Text {
                text: "Configure launcher appearance and front-end behaviour. Runtime-backed settings will connect as those services are implemented."
                color: Theme.textMuted
                font.pixelSize: 12
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12

            XPanel {
                Layout.preferredWidth: 220
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    Repeater {
                        model: root.categories

                        delegate: Rectangle {
                            required property int index
                            required property string modelData

                            Layout.fillWidth: true
                            Layout.preferredHeight: 46
                            radius: 8
                            color: root.categoryIndex === index ? Theme.accentSoft
                                 : categoryMouse.containsMouse ? Theme.surfaceHover
                                 : "transparent"
                            border.width: root.categoryIndex === index ? 1 : 0
                            border.color: Theme.accent

                            Text {
                                anchors.fill: parent
                                anchors.leftMargin: 14
                                verticalAlignment: Text.AlignVCenter
                                text: modelData
                                color: root.categoryIndex === index ? Theme.text : Theme.textMuted
                                font.pixelSize: 13
                                font.weight: root.categoryIndex === index ? Font.DemiBold : Font.Normal
                            }

                            MouseArea {
                                id: categoryMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.categoryIndex = index
                            }
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            XPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true

                StackLayout {
                    anchors.fill: parent
                    anchors.margins: 18
                    currentIndex: root.categoryIndex

                    // General
                    ScrollView {
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: parent.width
                            spacing: 14

                            Text { text: "General"; color: Theme.text; font.pixelSize: 22; font.weight: Font.DemiBold }
                            Text { text: "Core launcher behaviour and convenience options."; color: Theme.textMuted; font.pixelSize: 11 }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Launcher Language"; color: Theme.text; font.pixelSize: 12; font.weight: Font.Medium }
                                    Text { text: "Language used by the Xenon front-end."; color: Theme.textMuted; font.pixelSize: 10 }
                                }
                                ComboBox { Layout.preferredWidth: 220; model: ["English (UK)", "English (US)"] }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Compact Mode"; color: Theme.text; font.pixelSize: 12; font.weight: Font.Medium }
                                    Text { text: "Reduce spacing and panel padding."; color: Theme.textMuted; font.pixelSize: 10 }
                                }
                                Switch { checked: false }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Interface Animations"; color: Theme.text; font.pixelSize: 12; font.weight: Font.Medium }
                                    Text { text: "Enable small navigation and hover transitions."; color: Theme.textMuted; font.pixelSize: 10 }
                                }
                                Switch { checked: true }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Startup Page"; color: Theme.text; font.pixelSize: 12; font.weight: Font.Medium }
                                    Text { text: "Page shown when Xenon starts."; color: Theme.textMuted; font.pixelSize: 10 }
                                }
                                ComboBox { Layout.preferredWidth: 220; model: ["Library", "Modules", "Profiles"] }
                            }

                            Item { Layout.fillHeight: true }
                        }
                    }

                    // Appearance
                    ScrollView {
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: parent.width
                            spacing: 14

                            Text { text: "Appearance"; color: Theme.text; font.pixelSize: 22; font.weight: Font.DemiBold }
                            Text { text: "Theme selection lives here rather than in the main header."; color: Theme.textMuted; font.pixelSize: 11 }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                rowSpacing: 12
                                columnSpacing: 12

                                Repeater {
                                    model: root.themeChoices

                                    delegate: Rectangle {
                                        required property var modelData

                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 145
                                        radius: 10
                                        color: Theme.current === modelData.id ? Theme.accentSoft : Theme.surfaceAlt
                                        border.width: Theme.current === modelData.id ? 2 : 1
                                        border.color: Theme.current === modelData.id ? Theme.accent : Theme.border

                                        ColumnLayout {
                                            anchors.fill: parent
                                            anchors.margins: 14
                                            spacing: 8

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: modelData.name
                                                    color: Theme.text
                                                    font.pixelSize: 15
                                                    font.weight: Font.DemiBold
                                                }
                                                StatusPill {
                                                    visible: Theme.current === modelData.id
                                                    label: "Selected"
                                                    tone: Theme.accent
                                                }
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                text: modelData.description
                                                color: Theme.textMuted
                                                wrapMode: Text.WordWrap
                                                font.pixelSize: 10
                                            }

                                            RowLayout {
                                                spacing: 6
                                                Repeater {
                                                    model: modelData.id === "light"
                                                        ? ["#FFFFFF", "#F3F7FA", "#27D8C5", "#12213D"]
                                                        : modelData.id === "xenon-green"
                                                          ? ["#080E0B", "#16221A", "#4DF069", "#F5FAF6"]
                                                          : modelData.id === "industrial-amber"
                                                            ? ["#111312", "#222622", "#F4AE2D", "#F2F0E8"]
                                                            : ["#04111D", "#0E2637", "#27DDF4", "#F4F9FC"]
                                                    delegate: Rectangle {
                                                        required property string modelData
                                                        width: 28
                                                        height: 28
                                                        radius: 14
                                                        color: modelData
                                                        border.width: 1
                                                        border.color: Theme.border
                                                    }
                                                }
                                            }

                                            Item { Layout.fillHeight: true }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                Theme.setTheme(modelData.id)
                                                launcherBridge.themeId = modelData.id
                                            }
                                        }
                                    }
                                }
                            }

                            Item { Layout.fillHeight: true }
                        }
                    }

                    // Paths
                    ScrollView {
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: parent.width
                            spacing: 12

                            Text { text: "Paths"; color: Theme.text; font.pixelSize: 22; font.weight: Font.DemiBold }
                            Text { text: "Default locations used by the launcher. These are front-end fields until the filesystem service is connected."; color: Theme.textMuted; font.pixelSize: 11 }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                            Repeater {
                                model: [
                                    ["Game Library", ""],
                                    ["Save Data", ""],
                                    ["Modules", ""],
                                    ["DLC Import Staging", ""]
                                ]

                                delegate: ColumnLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 4
                                    Text { text: modelData[0]; color: Theme.text; font.pixelSize: 11; font.weight: Font.Medium }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        TextField {
                                            Layout.fillWidth: true
                                            text: modelData[1]
                                            placeholderText: "Not configured"
                                            placeholderTextColor: Theme.textMuted
                                            color: Theme.text
                                            background: Rectangle { radius: 7; color: Theme.input; border.width: 1; border.color: Theme.border }
                                        }
                                        XButton { text: "Browse"; onClicked: launcherBridge.notifyUnavailable("Browse " + modelData[0] + " Path") }
                                    }
                                }
                            }

                            Item { Layout.fillHeight: true }
                        }
                    }

                    // Runtime
                    ScrollView {
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: parent.width
                            spacing: 14

                            Text { text: "Runtime"; color: Theme.text; font.pixelSize: 22; font.weight: Font.DemiBold }
                            Text { text: "Defaults exposed to modules once runtime integration is connected."; color: Theme.textMuted; font.pixelSize: 11 }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                            RowLayout {
                                Layout.fillWidth: true
                                Text { Layout.fillWidth: true; text: "Preferred Graphics Backend"; color: Theme.text; font.pixelSize: 12 }
                                ComboBox { Layout.preferredWidth: 220; model: ["Automatic", "Vulkan", "Direct3D 12"] }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Text { Layout.fillWidth: true; text: "Target Architecture"; color: Theme.text; font.pixelSize: 12 }
                                ComboBox { Layout.preferredWidth: 220; model: ["Automatic", "x86-64", "ARM64"] }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Offline Mode"; color: Theme.text; font.pixelSize: 12 }
                                    Text { text: "Disable online service integration by default."; color: Theme.textMuted; font.pixelSize: 10 }
                                }
                                Switch { checked: true }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Verify Compatibility Before Launch"; color: Theme.text; font.pixelSize: 12 }
                                    Text { text: "Ask the selected module to verify imported content before launching."; color: Theme.textMuted; font.pixelSize: 10 }
                                }
                                Switch { checked: true }
                            }

                            Item { Layout.fillHeight: true }
                        }
                    }

                    // Updates
                    ScrollView {
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: parent.width
                            spacing: 14

                            Text { text: "Updates"; color: Theme.text; font.pixelSize: 22; font.weight: Font.DemiBold }
                            Text { text: "Xenon only updates launcher/runtime software, open modules, and metadata. It does not download commercial game content or DLC."; color: Theme.textMuted; wrapMode: Text.WordWrap; font.pixelSize: 11 }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Check for Xenon Updates"; color: Theme.text; font.pixelSize: 12 }
                                    Text { text: "Check launcher and runtime releases."; color: Theme.textMuted; font.pixelSize: 10 }
                                }
                                XButton { text: "Check Now"; onClicked: launcherBridge.notifyUnavailable("Check for Xenon Updates") }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Check Module Updates"; color: Theme.text; font.pixelSize: 12 }
                                    Text { text: "Check installed open-source game modules for updates."; color: Theme.textMuted; font.pixelSize: 10 }
                                }
                                Switch { checked: true }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Pre-release Builds"; color: Theme.text; font.pixelSize: 12 }
                                    Text { text: "Include development and preview builds."; color: Theme.textMuted; font.pixelSize: 10 }
                                }
                                Switch { checked: false }
                            }

                            Item { Layout.fillHeight: true }
                        }
                    }

                    // About
                    ScrollView {
                        clip: true
                        contentWidth: availableWidth

                        ColumnLayout {
                            width: parent.width
                            spacing: 12

                            Text { text: "About Xenon"; color: Theme.text; font.pixelSize: 22; font.weight: Font.DemiBold }
                            Text { text: "Xenon Recomp"; color: Theme.accent; font.pixelSize: 16; font.weight: Font.DemiBold }
                            Text { text: "Launcher version " + launcherBridge.version; color: Theme.textMuted; font.pixelSize: 11 }
                            StatusPill {
                                visible: launcherBridge.testMode
                                label: "TEST MODE ENABLED • FICTIONAL UI DATA"
                                tone: Theme.warning
                            }
                            Text {
                                visible: launcherBridge.testMode
                                Layout.fillWidth: true
                                text: "Test mode is a compile-time developer switch in launcher/src/launcher_config.hpp. Normal builds start with an empty library and module registry."
                                color: Theme.warning
                                wrapMode: Text.WordWrap
                                font.pixelSize: 10
                            }
                            Text {
                                Layout.fillWidth: true
                                text: "A modular recompilation runtime and launcher designed to keep game-specific behaviour in independent modules while Xenon provides shared runtime, compatibility, content, profile, and platform services."
                                color: Theme.textMuted
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                lineHeight: 1.25
                            }
                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
                            Text { text: "Host architectures: x86-64, ARM64"; color: Theme.text; font.pixelSize: 11 }
                            Text { text: "Planned platforms: Windows, Linux, Android"; color: Theme.text; font.pixelSize: 11 }
                            Text { text: "Graphics backends: Vulkan, Direct3D 12"; color: Theme.text; font.pixelSize: 11 }
                            Item { Layout.fillHeight: true }
                        }
                    }
                }
            }
        }
    }
}
