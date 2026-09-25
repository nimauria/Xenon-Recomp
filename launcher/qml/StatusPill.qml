import QtQuick
import QtQuick.Layouts

Rectangle {
    id: root

    property string label: ""
    // Prefer semantic states at call sites. `tone` remains available for
    // badges that are informational rather than a runtime state.
    property string status: "neutral" // active, ready, waiting, degraded, unavailable, notImplemented, error
    property color tone: Theme.textMuted
    readonly property color effectiveTone: root.status === "neutral" ? root.tone : root.statusTone(root.status)

    implicitHeight: 28
    implicitWidth: row.implicitWidth + Theme.spaceLg
    radius: implicitHeight / 2
    color: Theme.highContrast
        ? Theme.surfaceAlt
        : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)
    border.width: Theme.borderWidth
    border.color: Theme.border

    Accessible.role: Accessible.StaticText
    Accessible.name: root.label
    Accessible.description: root.status === "neutral" ? "" : "Status: " + root.status

    function statusTone(value) {
        switch (String(value)) {
        case "active": return Theme.success
        case "ready": return Theme.accent
        case "waiting": case "degraded": return Theme.warning
        case "error": return Theme.danger
        default: return Theme.textMuted
        }
    }

    function statusIcon() {
        if (root.status === "active" || root.status === "ready")
            return "qrc:/theme-art/decor/neutral/status_ready.svg"
        if (root.status === "waiting" || root.status === "degraded")
            return "qrc:/theme-art/decor/neutral/status_processing.svg"
        if (root.status === "error")
            return "qrc:/theme-art/decor/neutral/status_error.svg"
        if (root.status === "unavailable" || root.status === "notImplemented")
            return "qrc:/theme-art/decor/neutral/status_offline.svg"

        var toneString = String(root.effectiveTone)
        if (toneString === String(Theme.success))
            return "qrc:/theme-art/decor/neutral/status_ready.svg"
        if (toneString === String(Theme.warning))
            return "qrc:/theme-art/decor/neutral/status_processing.svg"
        if (toneString === String(Theme.danger))
            return "qrc:/theme-art/decor/neutral/status_error.svg"
        if (toneString === String(Theme.textMuted))
            return "qrc:/theme-art/decor/neutral/status_offline.svg"
        return ""
    }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceSm

        Image {
            id: statusImage
            Layout.preferredWidth: 12
            Layout.preferredHeight: 12
            source: root.statusIcon()
            visible: source.toString().length > 0
            fillMode: Image.PreserveAspectFit
            smooth: true
        }

        Rectangle {
            visible: !statusImage.visible
            width: 8
            height: 8
            radius: 4
            color: root.effectiveTone
        }

        Text {
            text: root.label
            color: root.effectiveTone
            font.pixelSize: Theme.typeCaption
            font.weight: Font.Medium
        }
    }
}
