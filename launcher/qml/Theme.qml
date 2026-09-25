pragma Singleton

import QtQuick

QtObject {
    id: root

    // The backend owns the actual theme catalogue. This fallback palette only
    // exists so the first QML frame can be constructed before Main applies the
    // persisted backend definition.
    property var palette: ({
        name: "Xenon Dark", dark: true,
        window: "#07131D", header: "#081822", sidebar: "#091923",
        surface: "#0D202C", surfaceAlt: "#122936", surfaceHover: "#173544",
        surfaceRaised: "#112633", border: "#294757", divider: "#213945",
        text: "#F4F8FA", textMuted: "#A9BAC4",
        accent: "#35D7EA", accentStrong: "#15B8CF", accentSoft: "#133943", accentText: "#031014",
        success: "#45D995", warning: "#E7B54B", danger: "#F06470",
        input: "#0A1B26", overlay: "#CC020910", decorFamily: "blue", defaultBackdrop: "orbit"
    })
    property var activeAccentDefinition: ({ id: "default", usesCustom: false })

    property string current: "system"
    property string effectiveThemeId: "xenon-dark"
    property string accentPreset: "default"
    property string cornerStyle: "rounded"
    property string decorLevel: "Balanced"
    property bool systemDark: false
    property bool systemHighContrast: false
    property bool userHighContrast: false
    property bool enhancedFocus: false
    property bool reduceMotion: false
    property bool handheld: false
    property real textScale: 1.0
    property real panelOpacity: 0.94

    readonly property bool usesCustomAccent: accentPreset !== "default"
        && activeAccentDefinition !== undefined
        && activeAccentDefinition.accent !== undefined
    readonly property string decorFamily: palette.decorFamily !== undefined ? String(palette.decorFamily) : "neutral"

    readonly property string name: current === "system" ? "System" : String(palette.name || "Xenon")
    readonly property bool dark: Boolean(palette.dark)
    readonly property color window: palette.window
    readonly property color header: palette.header
    readonly property color sidebar: palette.sidebar
    readonly property color surface: palette.surface
    readonly property color surfaceAlt: palette.surfaceAlt
    readonly property color surfaceHover: palette.surfaceHover
    readonly property color surfaceRaised: palette.surfaceRaised
    readonly property bool highContrast: systemHighContrast || userHighContrast
    readonly property color border: highContrast ? (dark ? "#E8EEF1" : "#1A2630") : palette.border
    readonly property color divider: highContrast ? border : palette.divider
    readonly property color text: palette.text
    readonly property color textMuted: highContrast ? palette.text : palette.textMuted
    readonly property color accent: usesCustomAccent ? activeAccentDefinition.accent : palette.accent
    readonly property color accentStrong: usesCustomAccent ? activeAccentDefinition.strong : palette.accentStrong
    readonly property color accentSoft: usesCustomAccent
        ? (dark ? activeAccentDefinition.softDark : activeAccentDefinition.softLight)
        : palette.accentSoft
    readonly property color accentText: usesCustomAccent ? activeAccentDefinition.text : palette.accentText
    readonly property color success: palette.success
    readonly property color warning: palette.warning
    readonly property color danger: palette.danger
    readonly property color input: palette.input
    readonly property color overlay: palette.overlay
    readonly property color focusRing: accent

    readonly property int spaceXs: 4
    readonly property int spaceSm: 8
    readonly property int spaceMd: 12
    readonly property int spaceLg: 16
    readonly property int spaceXl: 24
    readonly property int space2Xl: 32
    readonly property int space3Xl: 48

    // Shared presentation values keep interaction feedback and container
    // hierarchy coherent without turning Theme into a second styling engine.
    readonly property real disabledOpacity: 0.58
    readonly property real secondaryOpacity: 0.78
    readonly property real hoverOpacity: 0.10
    readonly property real pressedOpacity: 0.16
    readonly property int motionFast: reduceMotion ? 0 : 100
    readonly property int motionNormal: reduceMotion ? 0 : 150
    readonly property int motionSlow: reduceMotion ? 0 : 180
    readonly property int iconSizeSmall: 16
    readonly property int iconSize: 20
    readonly property int iconSizeLarge: 24

    readonly property real captionScale: 1.0 + Math.max(0, textScale - 1.0) * 0.80
    readonly property real bodyScale: 1.0 + Math.max(0, textScale - 1.0) * 0.66
    readonly property real headingScale: 1.0 + Math.max(0, textScale - 1.0) * 0.38
    readonly property real displayScale: 1.0 + Math.max(0, textScale - 1.0) * 0.28

    readonly property int controlHeight: Math.round((handheld ? 46 : 40) + Math.max(0, bodyScale - 1.0) * 18)
    readonly property int controlHeightLarge: Math.round((handheld ? 54 : 48) + Math.max(0, bodyScale - 1.0) * 22)
    readonly property int pageMargin: textScale >= 1.75 ? 18 : 20
    readonly property int contentMaxWidth: textScale >= 1.75 ? 1180 : 1080
    readonly property int sidebarWidth: Math.round(184 + Math.max(0, bodyScale - 1.0) * 22)
    readonly property int sidebarCompactWidth: Math.round(68 + Math.max(0, bodyScale - 1.0) * 4)

    readonly property real typeCaption: 12 * captionScale
    readonly property real typeBody: 14 * bodyScale
    readonly property real typeBodyLarge: 18 * bodyScale
    readonly property real typeSubtitle: 20 * headingScale
    readonly property real typeTitle: 28 * headingScale
    readonly property real typeDisplay: 40 * displayScale

    readonly property real panelRadius: cornerStyle === "square" ? 4
                                            : cornerStyle === "soft" ? 8 : 12
    readonly property real cardRadius: panelRadius
    readonly property real dialogRadius: cornerStyle === "square" ? 6
                                             : cornerStyle === "soft" ? 10 : 14
    readonly property real controlRadius: cornerStyle === "square" ? 4
                                              : cornerStyle === "soft" ? 7 : 9
    readonly property real borderWidth: highContrast ? 2 : 1
    readonly property real focusWidth: highContrast ? 3 : enhancedFocus ? 4 : 2

    function applyAppearance(themeId, resolvedThemeId, themeDefinition, accentId, accentDefinition) {
        current = themeId || "system"
        effectiveThemeId = resolvedThemeId || "xenon-dark"
        if (themeDefinition && themeDefinition.window !== undefined)
            palette = themeDefinition
        accentPreset = accentId || "default"
        activeAccentDefinition = accentDefinition || ({ id: "default", usesCustom: false })
    }

    function setCornerStyle(styleId) {
        if (["rounded", "soft", "square"].indexOf(styleId) !== -1)
            cornerStyle = styleId
    }

    function setSystemAppearance(isDark, highContrast) {
        systemDark = Boolean(isDark)
        systemHighContrast = Boolean(highContrast)
    }

    function setAccessibility(scale, highContrastOverride, enhancedFocusOverride, reduceMotionOverride) {
        textScale = Math.max(1.0, Math.min(Number(scale), 2.0))
        userHighContrast = Boolean(highContrastOverride)
        enhancedFocus = Boolean(enhancedFocusOverride)
        reduceMotion = Boolean(reduceMotionOverride)
    }

    function setAdvancedAppearance(level, opacity) {
        decorLevel = ["Minimal", "Balanced", "Full"].indexOf(level) !== -1 ? level : "Balanced"
        panelOpacity = Math.max(0.0, Math.min(Number(opacity), 1.0))
    }

    function decorAsset(asset) {
        if (["blue", "green", "amber"].indexOf(decorFamily) === -1)
            return ""
        return "qrc:/theme-art/decor/" + decorFamily + "/" + asset + "_" + decorFamily + ".svg"
    }
}
