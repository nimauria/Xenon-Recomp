import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Low-level Xenon tooling, only reachable when Settings > Advanced >
// Developer Mode is on. Every section here is backed by a real capability
// LauncherBridge already exposes; a subsystem with no real introspection
// available yet (CPU/GPU/Shaders/Memory - see the capability research this
// page was built from) is omitted rather than shown as a fake panel.
Item {
    id: root

    property int revision: 0
    readonly property var status: { var r = revision; return launcherBridge.gameStatus() }
    readonly property bool sessionActive: Boolean(root.status.running)
    readonly property var subsystems: root.status.subsystems || ({})
    readonly property var loadedXex: root.status.loadedXex || ({})
    readonly property var unresolvedImports: root.status.unresolvedImports || []

    Timer {
        interval: 1000
        running: root.visible
        repeat: true
        onTriggered: root.revision += 1
    }

    function subsystemTone(active) {
        return Boolean(active) ? Theme.success : Theme.textMuted
    }

    XSettingsPage {
        anchors.fill: parent
        title: "Developer"
        description: "Runtime, kernel and input introspection for Xenon development. Hidden unless Developer Mode is on."

        Text {
            Layout.fillWidth: true
            text: "Runtime session"
            color: Theme.text
            font.pixelSize: Theme.typeBodyLarge
            font.weight: Font.DemiBold
        }

        XSettingsCard {
            title: "Session state"
            description: root.sessionActive ? String(root.status.stateName || "running") : "No game session is currently running."
            StatusPill { label: root.sessionActive ? "Active" : "Idle"; tone: root.sessionActive ? Theme.success : Theme.textMuted }
        }

        XSettingsCard {
            visible: root.sessionActive
            title: "Subsystems"
            description: "Reported by the active runtime host session."
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                XInfoRow { label: "Memory"; value: Boolean(root.subsystems.memory) ? "Active" : "Inactive" }
                XInfoRow { label: "Filesystem"; value: Boolean(root.subsystems.filesystem) ? "Active" : "Inactive" }
                XInfoRow { label: "Input"; value: Boolean(root.subsystems.input) ? "Active" : "Inactive" }
                XInfoRow { label: "GPU"; value: Boolean(root.subsystems.gpu) ? "Active" : "Inactive" }
                XInfoRow { label: "XAM"; value: Boolean(root.subsystems.xam) ? "Active" : "Inactive" }
            }
        }

        XSettingsCard {
            visible: root.sessionActive && Boolean(root.loadedXex.loaded)
            title: "Loaded XEX"
            description: "Title/image data reported by the current session."
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                XInfoRow { label: "Title ID"; value: String(root.loadedXex.titleId || "Unknown") }
                XInfoRow { label: "Entry point"; value: String(root.loadedXex.entryPoint || "") }
                XInfoRow { label: "Image base"; value: String(root.loadedXex.imageBase || "") }
                XInfoRow { label: "Base version"; value: String(root.loadedXex.baseVersion || "") }
                XInfoRow { label: "Effective version"; value: String(root.loadedXex.effectiveVersion || "") }
                XInfoRow { label: "Title update applied"; value: Boolean(root.loadedXex.titleUpdateApplied) ? "Yes" : "No" }
            }
        }

        XSettingsCard {
            visible: root.sessionActive && root.unresolvedImports.length > 0
            title: "Unresolved kernel imports"
            description: root.unresolvedImports.length + " import" + (root.unresolvedImports.length === 1 ? "" : "s") + " could not be resolved."
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Repeater {
                    model: root.unresolvedImports
                    delegate: Text {
                        required property var modelData
                        Layout.fillWidth: true
                        text: String(modelData.library || "?") + "!" + String(modelData.symbol || modelData.ordinal || "?")
                        color: Theme.warning
                        font.family: "monospace"
                        font.pixelSize: Theme.typeCaption
                    }
                }
            }
        }

        XSettingsCard {
            title: "Last error"
            visible: root.sessionActive && String(root.status.lastError || "").length > 0
            description: String(root.status.lastError || "")
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider; Layout.topMargin: Theme.spaceSm }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceSm
            text: "Input"
            color: Theme.text
            font.pixelSize: Theme.typeBodyLarge
            font.weight: Font.DemiBold
        }

        XSettingsCard {
            title: "Xenon Input"
            description: launcherBridge.inputStatus()
            actionWidth: 300
            XInfoRow { label: "Module API"; value: "v" + String(launcherBridge.inputModuleApiInfo().version || 0) }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider; Layout.topMargin: Theme.spaceSm }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceSm
            text: "Runtime capabilities"
            color: Theme.text
            font.pixelSize: Theme.typeBodyLarge
            font.weight: Font.DemiBold
        }

        XSettingsCard {
            title: "Compiled subsystems"
            description: "Which Xenon runtime subsystems this build was compiled with."
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                XInfoRow { label: "Graphics"; value: launcherBridge.runtimeCapability("graphicsCompiled") ? "Compiled" : "Not compiled" }
                XInfoRow { label: "Audio"; value: launcherBridge.runtimeCapability("audioCompiled") ? "Compiled" : "Not compiled" }
                XInfoRow { label: "Input"; value: launcherBridge.runtimeCapability("inputCompiled") ? "Compiled" : "Not compiled" }
                XInfoRow { label: "Network"; value: launcherBridge.runtimeCapability("networkCompiled") ? "Compiled" : "Not compiled" }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider; Layout.topMargin: Theme.spaceSm }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceSm
            text: "Logging"
            color: Theme.text
            font.pixelSize: Theme.typeBodyLarge
            font.weight: Font.DemiBold
        }

        XPanel {
            Layout.fillWidth: true
            implicitHeight: Math.min(320, logColumn.implicitHeight + Theme.spaceLg * 2)
            color: Theme.highContrast ? Theme.surfaceAlt : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)

            ScrollView {
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                clip: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    id: logColumn
                    width: parent.width
                    Text {
                        Layout.fillWidth: true
                        text: launcherBridge.runtimeLogTail().length > 0 ? launcherBridge.runtimeLogTail() : "No runtime host log is available yet."
                        color: Theme.textMuted
                        font.family: "monospace"
                        font.pixelSize: Theme.typeCaption
                        wrapMode: Text.WrapAnywhere
                        textFormat: Text.PlainText
                    }
                }
            }
        }
    }
}
