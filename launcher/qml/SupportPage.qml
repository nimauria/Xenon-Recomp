import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Troubleshooting/diagnostics centre for normal users - every action here
// calls a real, already-implemented backend capability (support bundle
// creation, log access, per-game/per-module verification, update checks).
// Nothing here is faked: a capability with no real backend yet (graphics,
// audio and network diagnostics) is reported as unavailable rather than
// shown as if it worked.
Item {
    id: root

    property int libraryRevision: 0
    property int moduleRevision: 0
    property int runtimeRevision: 0
    readonly property var libraryEntries: { var r = libraryRevision; return launcherBridge.libraryEntries() }
    readonly property var moduleEntries: { var r = moduleRevision; return launcherBridge.moduleEntries() }

    function refresh() {
        libraryRevision += 1
        moduleRevision += 1
        runtimeRevision += 1
    }

    function serviceLabel(service) {
        var r = root.runtimeRevision
        if (launcherBridge.runtimeCapability(service)) return "Active"
        if (launcherBridge.runtimeCapability(service + "Compiled"))
            return launcherBridge.backendConnected ? "Ready" : "Built • host missing"
        return service === "network" ? "In development" : "Not built"
    }

    function serviceTone(service) {
        var r = root.runtimeRevision
        if (launcherBridge.runtimeCapability(service)) return Theme.success
        if (launcherBridge.runtimeCapability(service + "Compiled"))
            return launcherBridge.backendConnected ? Theme.accent : Theme.warning
        return Theme.textMuted
    }

    Connections {
        target: launcherBridge
        function onLibraryChanged() { root.libraryRevision += 1 }
        function onModulesChanged() { root.moduleRevision += 1 }
        function onBackendConnectedChanged() { root.runtimeRevision += 1 }
        function onSessionChanged() { root.runtimeRevision += 1 }
        function onInputChanged() { root.runtimeRevision += 1 }
    }

    Timer {
        interval: 1000
        running: root.visible
        repeat: true
        onTriggered: root.runtimeRevision += 1
    }

    XSettingsPage {
        anchors.fill: parent
        title: "Support"
        description: "Troubleshoot Xenon, verify installed content and reach the community without digging through AppData or GitHub by hand."

        XSettingsCard {
            title: "System check"
            description: launcherBridge.backendConnected
                ? "The launcher found xenon_runtime_host beside the UI and can start real game sessions."
                : "The launcher UI is running, but the runtime host executable is missing from this build/output folder."
            StatusPill {
                label: launcherBridge.backendConnected ? "Runtime host ready" : "Launcher only"
                tone: launcherBridge.backendConnected ? Theme.accent : Theme.warning
            }
            XButton { text: "Copy system summary"; onClicked: { launcherBridge.copyText(launcherBridge.userDiagnostics()); launcherBridge.notify("Summary copied", "A system summary was copied to the clipboard.") } }
        }

        XSettingsCard {
            title: "Controller diagnostics"
            description: launcherBridge.inputAvailable()
                ? "Xenon Input initialized successfully. No connected controller is required for the service itself to be ready."
                : (launcherBridge.runtimeCapability("inputCompiled")
                    ? "Xenon Input is built, but the host input backend did not initialize."
                    : "Xenon Input is not included in this build.")
            StatusPill {
                label: launcherBridge.inputAvailable() ? "Ready"
                    : (launcherBridge.runtimeCapability("inputCompiled") ? "Built • unavailable" : "Not built")
                tone: launcherBridge.inputAvailable() ? Theme.accent
                    : (launcherBridge.runtimeCapability("inputCompiled") ? Theme.warning : Theme.textMuted)
            }
            XButton {
                text: launcherBridge.inputAvailable() ? "Refresh input" : "Retry input"
                onClicked: launcherBridge.inputAvailable() ? launcherBridge.refreshInputDevices() : launcherBridge.reconfigureInput()
            }
            XButton { text: "Copy input diagnostics"; onClicked: { launcherBridge.copyText(JSON.stringify(launcherBridge.inputDiagnostics(), null, 2)); launcherBridge.notify("Copied", "Controller diagnostics were copied to the clipboard.") } }
        }

        XSettingsCard {
            title: "Runtime subsystem readiness"
            description: "Active means the current game session initialized the subsystem. Ready means it is compiled and the runtime host can start it. Detailed adapter/device inspectors can be added independently later."
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                RowLayout {
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: "Graphics"; color: Theme.text }
                    StatusPill { label: root.serviceLabel("graphics"); tone: root.serviceTone("graphics") }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: "Audio"; color: Theme.text }
                    StatusPill { label: root.serviceLabel("audio"); tone: root.serviceTone("audio") }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { Layout.fillWidth: true; text: "Network"; color: Theme.text }
                    StatusPill { label: root.serviceLabel("network"); tone: root.serviceTone("network") }
                }
            }
        }

        XSettingsCard {
            title: "Check for launcher updates"
            description: String(launcherBridge.launcherUpdateState().statusMessage || "Ready to check for updates.")
            XButton {
                text: "Check for updates"
                enabled: !Boolean(launcherBridge.launcherUpdateState().busy)
                onClicked: launcherBridge.requestLauncherUpdateCheck()
            }
        }

        XSettingsCard {
            title: "Support bundle"
            description: "Create a privacy-sanitized ZIP for a GitHub issue or Discord support post. Local paths and profile names are redacted."
            XButton {
                text: "Create Support Bundle"
                variant: "primary"
                onClicked: {
                    var path = launcherBridge.createSupportBundle()
                    if (String(path).length > 0) launcherBridge.openFolder(launcherBridge.supportBundleDirectory())
                }
            }
            XButton { text: "Open Logs"; onClicked: launcherBridge.openFolder(launcherBridge.diagnosticsDirectory()) }
        }

        XSettingsCard {
            title: "Open documentation"
            description: "Read the project README and setup documentation on GitHub."
            XButton { text: "Open Documentation"; onClicked: launcherBridge.openExternalUrl("https://github.com/nimauria/Xenon-Recomp#readme") }
        }

        XSettingsCard {
            title: "Report an issue"
            description: "File a bug report or feature request on the Xenon-Recomp GitHub issue tracker."
            XButton { text: "Report Issue"; onClicked: launcherBridge.openExternalUrl("https://github.com/nimauria/Xenon-Recomp/issues/new") }
        }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceMd
            text: "Verify installed games"
            color: Theme.text
            font.pixelSize: Theme.typeBodyLarge
            font.weight: Font.DemiBold
        }

        Repeater {
            model: root.libraryEntries
            delegate: XSettingsCard {
                required property var modelData
                title: String(modelData.title || modelData.gameId || "Game")
                description: "Confirms the game's content path still exists on disk."
                XButton {
                    text: "Verify"
                    onClicked: launcherBridge.verifyLibraryEntry(modelData.gameId)
                }
            }
        }

        Text {
            visible: root.libraryEntries.length === 0
            Layout.fillWidth: true
            text: "No games in the library yet."
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
        }

        Text {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceMd
            text: "Repair installed modules"
            color: Theme.text
            font.pixelSize: Theme.typeBodyLarge
            font.weight: Font.DemiBold
        }

        Repeater {
            model: root.moduleEntries
            delegate: XSettingsCard {
                required property var modelData
                title: String(modelData.moduleName || modelData.moduleId || "Module")
                description: "Re-validates the module manifest and its declared runtime API version."
                XButton {
                    text: "Verify / Repair"
                    onClicked: launcherBridge.verifyModule(modelData.moduleId)
                }
            }
        }

        Text {
            visible: root.moduleEntries.length === 0
            Layout.fillWidth: true
            text: "No modules installed yet."
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
        }
    }
}
