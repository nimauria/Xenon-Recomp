import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Xenon's online/multiplayer capability does not exist yet - this page says
// so plainly rather than faking a connected service. The one line that IS
// backed by a real check is Internet reachability (Qt's own OS-level
// QNetworkInformation backend, see LauncherBridge::networkReachabilityStatus).
Item {
    id: root

    property int reachabilityRevision: 0
    readonly property string reachability: { var r = reachabilityRevision; return launcherBridge.networkReachabilityStatus() }

    Timer {
        // Reachability can change while this page is open (Wi-Fi toggled,
        // cable unplugged); poll at a human-scale interval rather than
        // relying on a signal no backend here emits.
        interval: 5000
        running: root.visible
        repeat: true
        onTriggered: root.reachabilityRevision += 1
    }

    XSettingsPage {
        anchors.fill: parent
        title: "Network"
        description: "Xenon's online services are still in development. This page reflects the genuine current state rather than a placeholder."

        XSettingsCard {
            title: "Internet"
            description: "Detected via the operating system's own network reachability service."
            StatusPill {
                label: root.reachability === "online" ? "Connected"
                     : root.reachability === "offline" ? "Offline"
                     : "Unknown"
                tone: root.reachability === "online" ? Theme.success
                    : root.reachability === "offline" ? Theme.danger
                    : Theme.textMuted
            }
        }

        XSettingsCard {
            title: "Xenon Network Services"
            description: "Online identity, matchmaking and service-replacement infrastructure for Xbox 360 titles is planned but not implemented."
            StatusPill { label: "In development"; tone: Theme.warning }
        }

        XSettingsCard {
            title: "Multiplayer"
            description: "No title's networking has been reimplemented against a live service yet."
            StatusPill { label: "Not available"; tone: Theme.textMuted }
        }

        XSettingsCard {
            title: "Online identity"
            description: "Xbox Live-equivalent profile/identity services are not implemented."
            StatusPill { label: "Not available"; tone: Theme.textMuted }
        }
    }
}
