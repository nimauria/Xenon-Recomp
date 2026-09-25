import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// OS Internet reachability and Xenon Network readiness are deliberately
// separate: one is not proof of the other.
Item {
    id: root

    property int reachabilityRevision: 0
    property int networkRevision: 0
    readonly property string reachability: {
        var revision = reachabilityRevision
        return launcherBridge.networkReachabilityStatus()
    }
    readonly property var network: {
        var revision = networkRevision
        return launcherBridge.networkStatus()
    }

    function serviceStatus() {
        var state = String(root.network.connectionState || "disabled")
        if (state === "ready") return "active"
        if (state === "resolving" || state === "connecting" || state === "authenticating" ||
                state === "reconnecting") return "waiting"
        if (state === "degraded" || state === "connected_transport") return "degraded"
        if (state === "error") return "error"
        return "unavailable"
    }

    Connections {
        target: launcherBridge
        function onNetworkChanged() { root.networkRevision += 1 }
    }

    Timer {
        interval: 5000
        running: root.visible
        repeat: true
        onTriggered: root.reachabilityRevision += 1
    }

    XSettingsPage {
        anchors.fill: parent
        title: "Network"
        description: "Xenon Network is independent from Xbox Live. Offline use remains fully supported, and online state is reported only after real protocol negotiation."

        XSettingsCard {
            title: "Internet"
            description: "Host reachability reported by the operating system. This does not indicate Xenon Network readiness."
            StatusPill {
                label: root.reachability === "online" ? "Reachable"
                     : root.reachability === "offline" ? "Offline"
                     : "Unknown"
                status: root.reachability === "online" ? "ready"
                      : root.reachability === "offline" ? "error"
                      : "unavailable"
            }
        }

        XSettingsCard {
            title: "Xenon Network"
            description: Boolean(root.network.clientCompiled)
                ? "Client protocol v" + String(root.network.protocolVersion || 1) + ". Configure Development or Production under Settings > Network; no production endpoint is bundled."
                : "This build does not include the Xenon Network client."
            StatusPill {
                label: String(root.network.connectionLabel || "Disabled")
                status: root.serviceStatus()
            }
        }

        XSettingsCard {
            title: "Service readiness"
            description: "Transport connectivity, protocol compatibility, authentication, and realtime events are tracked independently."
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                XInfoRow { label: "Configured"; value: Boolean(root.network.configured) ? "Yes" : "No" }
                XInfoRow { label: "Service reachable"; value: Boolean(root.network.serviceReachable) ? "Yes" : "No" }
                XInfoRow { label: "Protocol compatible"; value: Boolean(root.network.protocolCompatible) ? "Yes" : "No" }
                XInfoRow { label: "Authentication"; value: String(root.network.authenticationState || "unauthenticated") }
                XInfoRow { label: "Realtime"; value: String(root.network.realtimeState || "disconnected") }
                XInfoRow { label: "Online services ready"; value: Boolean(root.network.onlineServicesReady) ? "Yes" : "No" }
            }
        }

        XSettingsCard {
            visible: launcherBridge.boolSetting("developer/modeEnabled", false)
            title: "Advanced diagnostics"
            description: "No credentials or session secrets are displayed."
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                XInfoRow { label: "Base endpoint"; value: String(root.network.baseEndpoint || "Not configured") }
                XInfoRow { label: "Capabilities"; value: (root.network.capabilities || []).join(", ") || "None negotiated" }
                XInfoRow { label: "Last contact"; value: String(root.network.lastSuccessfulContact || "Never") }
                XInfoRow { label: "Last error"; value: String(root.network.lastError || "none") }
                XInfoRow { label: "Requests"; value: String(root.network.requestsAttempted || 0) + " attempted / " + String(root.network.requestsFailed || 0) + " failed" }
            }
        }

        XSettingsCard {
            title: "Multiplayer data plane"
            description: "Gameplay packet transport, NAT traversal, relays, matchmaking infrastructure, and Xbox/XAM mappings are not implemented in this client-foundation pass."
            StatusPill { label: "Not available"; status: "notImplemented" }
        }
    }
}
