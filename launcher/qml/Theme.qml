pragma Singleton

import QtQuick

QtObject {
    id: root

    // ---------------------------------------------------------------------
    // Appearance state
    // ---------------------------------------------------------------------
    property string current: "system"
    property string accentPreset: "default"
    property string cornerStyle: "rounded"
    property bool systemDark: false
    property bool systemHighContrast: false
    property bool userHighContrast: false
    property real textScale: 1.0

    readonly property var themes: ({
        "xenon-dark": {
            name: "Xenon Dark",
            dark: true,
            window: "#07131D",
            header: "#081822",
            sidebar: "#091923",
            surface: "#0D202C",
            surfaceAlt: "#122936",
            surfaceHover: "#173544",
            surfaceRaised: "#112633",
            border: "#294757",
            divider: "#213945",
            text: "#F4F8FA",
            textMuted: "#A9BAC4",
            accent: "#35D7EA",
            accentStrong: "#15B8CF",
            accentSoft: "#133943",
            accentText: "#031014",
            success: "#45D995",
            warning: "#E7B54B",
            danger: "#F06470",
            input: "#0A1B26",
            overlay: "#CC020910"
        },
        "carbon": {
            name: "Carbon",
            dark: true,
            window: "#0D1114",
            header: "#11161A",
            sidebar: "#101519",
            surface: "#171D21",
            surfaceAlt: "#1D252A",
            surfaceHover: "#263137",
            surfaceRaised: "#1B2227",
            border: "#3B4A52",
            divider: "#2E3A40",
            text: "#F4F6F7",
            textMuted: "#B1BCC2",
            accent: "#7ED6C6",
            accentStrong: "#54BFAE",
            accentSoft: "#203A36",
            accentText: "#07110F",
            success: "#55D690",
            warning: "#E7B54B",
            danger: "#EF6970",
            input: "#131A1E",
            overlay: "#D006090B"
        },
        "industrial": {
            name: "Industrial",
            dark: true,
            window: "#141513",
            header: "#1A1C19",
            sidebar: "#181A17",
            surface: "#20231F",
            surfaceAlt: "#292D27",
            surfaceHover: "#333830",
            surfaceRaised: "#252923",
            border: "#5A5442",
            divider: "#454136",
            text: "#F2F0E9",
            textMuted: "#B6B1A6",
            accent: "#EFB23B",
            accentStrong: "#D79620",
            accentSoft: "#49391F",
            accentText: "#171006",
            success: "#62D58A",
            warning: "#EFB23B",
            danger: "#F06A60",
            input: "#1B1D1A",
            overlay: "#D00A0B0A"
        },
        "light": {
            name: "Light",
            dark: false,
            window: "#F4F7F9",
            header: "#FFFFFF",
            sidebar: "#F8FAFB",
            surface: "#FFFFFF",
            surfaceAlt: "#F0F4F6",
            surfaceHover: "#E8F0F3",
            surfaceRaised: "#FFFFFF",
            border: "#CAD8DF",
            divider: "#DEE7EB",
            text: "#142230",
            textMuted: "#60707C",
            accent: "#18C8BA",
            accentStrong: "#0AA99D",
            accentSoft: "#D9F5F1",
            accentText: "#071B18",
            success: "#159B61",
            warning: "#B87E0F",
            danger: "#D64654",
            input: "#FBFCFD",
            overlay: "#330B1B2A"
        }
    })

    readonly property var accentPresets: ({
        "cyan":    { accent: "#35D7EA", strong: "#15B8CF", softDark: "#133943", softLight: "#D9F5F8", text: "#031014" },
        "emerald": { accent: "#45D995", strong: "#22BA77", softDark: "#173B2A", softLight: "#DDF5E8", text: "#06120A" },
        "amber":   { accent: "#EFB23B", strong: "#D79620", softDark: "#49391F", softLight: "#FFF0D2", text: "#171006" },
        "violet":  { accent: "#A78BFA", strong: "#8667E8", softDark: "#312B4C", softLight: "#EEE9FF", text: "#100A1E" },
        "rose":    { accent: "#F2799A", strong: "#D95A7D", softDark: "#4B2834", softLight: "#FDE6EC", text: "#1A0710" }
    })

    readonly property string effectiveThemeId: current === "system"
        ? (systemDark ? "xenon-dark" : "light")
        : (themes[current] !== undefined ? current : "xenon-dark")
    readonly property var palette: themes[effectiveThemeId]
    readonly property var customAccent: accentPresets[accentPreset]
    readonly property bool usesCustomAccent: accentPreset !== "default" && customAccent !== undefined

    // ---------------------------------------------------------------------
    // Semantic colours
    // ---------------------------------------------------------------------
    readonly property string name: current === "system" ? "System" : palette.name
    readonly property bool dark: palette.dark
    readonly property color window: palette.window
    readonly property color header: palette.header
    readonly property color sidebar: palette.sidebar
    readonly property color surface: palette.surface
    readonly property color surfaceAlt: palette.surfaceAlt
    readonly property color surfaceHover: palette.surfaceHover
    readonly property color surfaceRaised: palette.surfaceRaised
    readonly property bool highContrast: systemHighContrast || userHighContrast
    readonly property color border: highContrast
        ? (dark ? "#E8EEF1" : "#1A2630")
        : palette.border
    readonly property color divider: highContrast ? border : palette.divider
    readonly property color text: palette.text
    readonly property color textMuted: highContrast ? palette.text : palette.textMuted
    readonly property color accent: usesCustomAccent ? customAccent.accent : palette.accent
    readonly property color accentStrong: usesCustomAccent ? customAccent.strong : palette.accentStrong
    readonly property color accentSoft: usesCustomAccent
        ? (dark ? customAccent.softDark : customAccent.softLight)
        : palette.accentSoft
    readonly property color accentText: usesCustomAccent ? customAccent.text : palette.accentText
    readonly property color success: palette.success
    readonly property color warning: palette.warning
    readonly property color danger: palette.danger
    readonly property color input: palette.input
    readonly property color overlay: palette.overlay
    readonly property color focusRing: accent

    // ---------------------------------------------------------------------
    // Design tokens: a small, consistent scale keeps every page aligned.
    // ---------------------------------------------------------------------
    readonly property int spaceXs: 4
    readonly property int spaceSm: 8
    readonly property int spaceMd: 12
    readonly property int spaceLg: 16
    readonly property int spaceXl: 24
    readonly property int space2Xl: 32

    // Accessibility text scaling is deliberately non-uniform. Windows applies
    // stronger scaling to smaller UI text than to large headings, and layouts
    // are expected to reflow rather than simply doubling every dimension.
    readonly property real captionScale: 1.0 + Math.max(0, textScale - 1.0) * 0.80
    readonly property real bodyScale: 1.0 + Math.max(0, textScale - 1.0) * 0.66
    readonly property real headingScale: 1.0 + Math.max(0, textScale - 1.0) * 0.38
    readonly property real displayScale: 1.0 + Math.max(0, textScale - 1.0) * 0.28

    readonly property int controlHeight: Math.round(40 + Math.max(0, bodyScale - 1.0) * 18)
    readonly property int controlHeightLarge: Math.round(48 + Math.max(0, bodyScale - 1.0) * 22)
    readonly property int pageMargin: textScale >= 1.75 ? 18 : 20
    readonly property int contentMaxWidth: textScale >= 1.75 ? 1180 : 1080
    readonly property int sidebarWidth: Math.round(184 + Math.max(0, bodyScale - 1.0) * 22)
    readonly property int sidebarCompactWidth: Math.round(68 + Math.max(0, bodyScale - 1.0) * 4)

    // Base sizes follow the Windows desktop UI range, while the scale factors
    // above keep 175-200% usable without making headings consume the screen.
    readonly property real typeCaption: 12 * captionScale
    readonly property real typeBody: 14 * bodyScale
    readonly property real typeBodyLarge: 18 * bodyScale
    readonly property real typeSubtitle: 20 * headingScale
    readonly property real typeTitle: 28 * headingScale
    readonly property real typeDisplay: 40 * displayScale

    readonly property real panelRadius: cornerStyle === "square" ? 4
                                            : cornerStyle === "soft" ? 8 : 12
    readonly property real controlRadius: cornerStyle === "square" ? 4
                                              : cornerStyle === "soft" ? 7 : 9
    readonly property real borderWidth: highContrast ? 2 : 1
    readonly property real focusWidth: highContrast ? 3 : 2

    function setTheme(themeId) {
        if (themeId === "system" || themes[themeId] !== undefined)
            current = themeId
    }

    function setAccent(accentId) {
        if (accentId === "default" || accentPresets[accentId] !== undefined)
            accentPreset = accentId
    }

    function setCornerStyle(styleId) {
        if (["rounded", "soft", "square"].indexOf(styleId) !== -1)
            cornerStyle = styleId
    }

    function setSystemAppearance(isDark, highContrast) {
        systemDark = isDark
        systemHighContrast = highContrast
    }

    function setAccessibility(scale, highContrastOverride) {
        textScale = Math.max(1.0, Math.min(Number(scale), 2.0))
        userHighContrast = Boolean(highContrastOverride)
    }
}
