import QtQuick

Item {
    id: root

    property real intensity: 1.0
    property bool subtle: false
    property string variant: "default"

    clip: true

    readonly property real strength: Math.max(0.0, Math.min(1.0, intensity))
    readonly property string resolvedVariant: {
        if (variant && variant !== "default")
            return variant
        if (Theme.effectiveThemeId === "industrial") return "orbit"
        if (Theme.effectiveThemeId === "carbon") return "nebula"
        if (Theme.effectiveThemeId === "xenon-dark") return "orbit"
        return "minimal"
    }

    function backgroundSource() {
        var id = Theme.effectiveThemeId
        var v = root.resolvedVariant
        if (id === "xenon-dark") {
            if (v === "tech") return "qrc:/theme-art/backgrounds/theme-xenon-dark-tech.png"
            if (v === "hud") return "qrc:/theme-art/backgrounds/theme-xenon-dark-hud.png"
            if (v === "orbit") return "qrc:/theme-art/backgrounds/theme-xenon-dark-orbit.png"
        }
        if (id === "industrial") {
            if (v === "tech") return "qrc:/theme-art/backgrounds/theme-industrial-tech.png"
            if (v === "orbit") return "qrc:/theme-art/backgrounds/theme-industrial-orbit.png"
        }
        if (id === "carbon") {
            if (v === "tech") return "qrc:/theme-art/backgrounds/theme-carbon-tech.png"
            if (v === "nebula") return "qrc:/theme-art/backgrounds/theme-carbon-nebula.png"
        }
        return ""
    }

    function sideDecorationSource() {
        if (Theme.effectiveThemeId === "industrial")
            return "qrc:/theme-art/decor/amber/side_strip_amber.svg"
        if (Theme.effectiveThemeId === "carbon")
            return "qrc:/theme-art/decor/green/side_strip_green.svg"
        if (Theme.effectiveThemeId === "xenon-dark")
            return "qrc:/theme-art/decor/blue/side_strip_blue.svg"
        return ""
    }

    function decorThemePath(asset) {
        if (Theme.effectiveThemeId === "industrial")
            return "qrc:/theme-art/decor/amber/" + asset + "_amber.svg"
        if (Theme.effectiveThemeId === "carbon")
            return "qrc:/theme-art/decor/green/" + asset + "_green.svg"
        if (Theme.effectiveThemeId === "xenon-dark")
            return "qrc:/theme-art/decor/blue/" + asset + "_blue.svg"
        return ""
    }

    Image {
        id: rasterBackdrop
        anchors.fill: parent
        source: root.backgroundSource()
        visible: source.toString().length > 0
        fillMode: Image.PreserveAspectCrop
        horizontalAlignment: Image.AlignLeft
        verticalAlignment: Image.AlignVCenter
        asynchronous: true
        cache: true
        smooth: true
        opacity: root.strength * (root.subtle ? 0.18 : 0.48)
    }

    // Keep text/panels readable even when a strong raster background is chosen.
    Rectangle {
        anchors.fill: parent
        visible: rasterBackdrop.visible
        color: Theme.window
        opacity: root.subtle ? 0.72 : 0.52
    }

    Image {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.min(120, parent.width * 0.12)
        source: root.sideDecorationSource()
        visible: source.toString().length > 0 && parent.width >= 520
        fillMode: Image.Stretch
        opacity: root.strength * (root.subtle ? 0.10 : 0.26)
        smooth: true
    }


    Image {
        anchors.left: parent.left
        anchors.top: parent.top
        width: Math.min(180, parent.width * 0.18)
        height: width
        source: root.decorThemePath("corner_top_left")
        visible: !root.subtle && source.toString().length > 0 && parent.width >= 760
        fillMode: Image.PreserveAspectFit
        opacity: root.strength * 0.30
        smooth: true
    }

    Image {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: Math.min(180, parent.width * 0.18)
        height: width
        source: root.decorThemePath("corner_bottom_right")
        visible: !root.subtle && source.toString().length > 0 && parent.width >= 760
        fillMode: Image.PreserveAspectFit
        opacity: root.strength * 0.24
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
        visible: !root.subtle && source.toString().length > 0 && parent.width >= 900
        fillMode: Image.PreserveAspectFit
        opacity: root.strength * 0.34
        smooth: true
    }

    Image {
        anchors.fill: parent
        source: Theme.effectiveThemeId === "light"
            ? "qrc:/theme-art/decor/neutral/hex_overlay.svg"
            : "qrc:/theme-art/decor/neutral/diagonal_lines_overlay.svg"
        fillMode: Image.Tile
        opacity: root.strength * (root.subtle ? 0.025 : 0.05)
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

            // A very light accent pass allows colour customisation to still be
            // visible over theme-specific raster artwork without recolouring it.
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

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
    }

    Connections {
        target: Theme
        function onEffectiveThemeIdChanged() { accentCanvas.requestPaint() }
        function onAccentChanged() { accentCanvas.requestPaint() }
    }
}
