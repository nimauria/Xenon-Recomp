import QtQuick

Item {
    id: root

    property real intensity: 1.0
    property bool subtle: false
    property string variant: "default"
    property string source: ""

    clip: true

    readonly property real strength: Math.max(0.0, Math.min(1.0, intensity))
    readonly property bool decorationsEnabled: Theme.decorLevel !== "Minimal"
    readonly property bool fullDecorations: Theme.decorLevel === "Full" && !root.subtle

    function decorThemePath(asset) {
        return Theme.decorAsset(asset)
    }

    CoverImage {
        id: rasterBackdrop
        anchors.fill: parent
        source: root.source
        visible: source.toString().length > 0
        fitMode: "cover"
        focalX: 0.0
        focalY: 0.5
        decodeHeadroom: 1.35
        opacity: root.strength * (root.subtle ? 0.28 : 0.72)
    }

    Rectangle {
        anchors.fill: parent
        visible: rasterBackdrop.visible
        color: Theme.window
        opacity: root.subtle ? 0.58 : 0.30
    }

    Image {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.min(140, parent.width * 0.13)
        source: root.decorThemePath("side_strip")
        visible: root.decorationsEnabled && source.toString().length > 0 && parent.width >= 520
        fillMode: Image.Stretch
        opacity: root.strength * (root.subtle ? 0.16 : 0.42)
        smooth: true
    }

    Image {
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.spaceSm
        width: 52
        height: Math.min(240, parent.height * 0.46)
        source: root.decorThemePath("rail_vertical")
        visible: root.fullDecorations && source.toString().length > 0 && parent.width >= 780
        fillMode: Image.Stretch
        opacity: root.strength * 0.30
        smooth: true
    }

    Image {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.spaceLg
        anchors.bottomMargin: Theme.space2Xl
        width: Math.min(190, parent.width * 0.18)
        height: width
        source: root.decorThemePath("hud_arc_left")
        visible: root.fullDecorations && source.toString().length > 0 && parent.width >= 820
        fillMode: Image.PreserveAspectFit
        opacity: root.strength * 0.28
        smooth: true
    }

    Image {
        anchors.left: parent.left
        anchors.top: parent.top
        width: Math.min(180, parent.width * 0.18)
        height: width
        source: root.decorThemePath("corner_top_left")
        visible: root.decorationsEnabled && !root.subtle && source.toString().length > 0 && parent.width >= 760
        fillMode: Image.PreserveAspectFit
        opacity: root.strength * 0.36
        smooth: true
    }

    Image {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Math.min(180, parent.width * 0.18)
        height: width
        source: root.decorThemePath("corner_bottom_right")
        visible: root.decorationsEnabled && !root.subtle && source.toString().length > 0 && parent.width >= 760
        fillMode: Image.PreserveAspectFit
        opacity: root.strength * 0.30
        smooth: true
    }

    Image {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.spaceXl
        anchors.bottomMargin: Theme.spaceLg
        width: 132
        height: 28
        source: root.decorThemePath("slashes")
        visible: root.fullDecorations && source.toString().length > 0 && parent.width >= 900
        fillMode: Image.PreserveAspectFit
        opacity: root.strength * 0.42
        smooth: true
    }

    Image {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: Theme.spaceXl
        anchors.topMargin: Theme.spaceLg
        width: Math.min(300, parent.width * 0.26)
        height: 18
        source: root.decorThemePath("divider_long")
        visible: root.fullDecorations && source.toString().length > 0 && parent.width >= 900
        fillMode: Image.PreserveAspectFit
        opacity: root.strength * 0.26
        smooth: true
    }

    Image {
        anchors.fill: parent
        source: Theme.effectiveThemeId === "light"
            ? "qrc:/theme-art/decor/neutral/hex_overlay.svg"
            : "qrc:/theme-art/decor/neutral/diagonal_lines_overlay.svg"
        fillMode: Image.Tile
        opacity: root.strength * (Theme.decorLevel === "Minimal" ? 0.02 : root.subtle ? 0.03 : 0.07)
        smooth: true
    }

    Image {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: Theme.space2Xl
        anchors.bottomMargin: Theme.space2Xl
        width: 54
        height: 54
        source: "qrc:/theme-art/decor/neutral/glow_dot.svg"
        visible: root.fullDecorations
        opacity: root.strength * 0.34
        smooth: true
    }

    Canvas {
        id: accentCanvas
        anchors.fill: parent
        opacity: root.strength * (root.subtle ? 0.18 : 0.34)

        function accent(alpha) {
            return Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, alpha)
        }

        onPaint: {
            var ctx = getContext("2d")
            ctx.clearRect(0, 0, width, height)
            if (width <= 0 || height <= 0)
                return

            var glow = ctx.createRadialGradient(width * 0.06, height * 0.86, 0,
                                                width * 0.06, height * 0.86,
                                                Math.max(width, height) * 0.64)
            glow.addColorStop(0.0, accent(0.18))
            glow.addColorStop(0.42, accent(0.055))
            glow.addColorStop(1.0, Qt.rgba(0, 0, 0, 0))
            ctx.fillStyle = glow
            ctx.fillRect(0, 0, width, height)

            if (!rasterBackdrop.visible) {
                ctx.strokeStyle = accent(0.13)
                ctx.lineWidth = 1.5
                ctx.beginPath()
                ctx.arc(-width * 0.04, height * 0.92,
                        Math.min(width, height) * 0.36,
                        Math.PI * 1.42, Math.PI * 1.94)
                ctx.stroke()
            }
        }

        onWidthChanged: canvasPaintThrottle.restart()
        onHeightChanged: canvasPaintThrottle.restart()
    }

    Timer {
        id: canvasPaintThrottle
        interval: 55
        repeat: false
        onTriggered: accentCanvas.requestPaint()
    }

    Connections {
        target: Theme
        function onEffectiveThemeIdChanged() { canvasPaintThrottle.restart() }
        function onAccentChanged() { canvasPaintThrottle.restart() }
    }
}
