pragma Singleton

import QtQuick

QtObject {
    id: root

    property string current: "xenon-cyan"

    readonly property var themes: ({
        "xenon-cyan": {
            name: "Xenon Cyan",
            dark: true,
            window: "#04111D",
            header: "#061827",
            sidebar: "#061724",
            surface: "#0A1E2D",
            surfaceAlt: "#0E2637",
            surfaceHover: "#123348",
            border: "#1D536F",
            divider: "#15394D",
            text: "#F4F9FC",
            textMuted: "#9FB4C4",
            accent: "#27DDF4",
            accentStrong: "#00BBD8",
            accentSoft: "#10394B",
            accentText: "#031117",
            success: "#35E59A",
            warning: "#F3BC43",
            danger: "#FF6672",
            input: "#081B29",
            overlay: "#CC020910"
        },
        "xenon-green": {
            name: "Xenon Green",
            dark: true,
            window: "#080E0B",
            header: "#0B120E",
            sidebar: "#09110D",
            surface: "#111A15",
            surfaceAlt: "#16221A",
            surfaceHover: "#1B2B21",
            border: "#28553A",
            divider: "#203F2D",
            text: "#F5FAF6",
            textMuted: "#A5B6AA",
            accent: "#4DF069",
            accentStrong: "#23D748",
            accentSoft: "#153B20",
            accentText: "#061209",
            success: "#4DF069",
            warning: "#FFC04B",
            danger: "#FF6C64",
            input: "#0E1712",
            overlay: "#D0060B08"
        },
        "industrial-amber": {
            name: "Industrial Amber",
            dark: true,
            window: "#111312",
            header: "#171918",
            sidebar: "#151716",
            surface: "#1B1E1C",
            surfaceAlt: "#222622",
            surfaceHover: "#2B302B",
            border: "#514A39",
            divider: "#3B392F",
            text: "#F2F0E8",
            textMuted: "#AAA89F",
            accent: "#F4AE2D",
            accentStrong: "#DE9108",
            accentSoft: "#443419",
            accentText: "#171006",
            success: "#54D77E",
            warning: "#F4AE2D",
            danger: "#FF6B61",
            input: "#171A18",
            overlay: "#D00A0B0A"
        },
        "light": {
            name: "Light",
            dark: false,
            window: "#F4F8FC",
            header: "#FFFFFF",
            sidebar: "#FAFCFE",
            surface: "#FFFFFF",
            surfaceAlt: "#F3F7FA",
            surfaceHover: "#EAF4F7",
            border: "#D8E3EB",
            divider: "#E5EDF3",
            text: "#12213D",
            textMuted: "#66758A",
            accent: "#27D8C5",
            accentStrong: "#08B9AA",
            accentSoft: "#DDF9F4",
            accentText: "#071C19",
            success: "#16BE72",
            warning: "#E3A51F",
            danger: "#E94F5E",
            input: "#F7FAFC",
            overlay: "#330B1B2A"
        }
    })

    readonly property var palette: themes[current] !== undefined ? themes[current] : themes["xenon-cyan"]

    readonly property string name: palette.name
    readonly property bool dark: palette.dark
    readonly property color window: palette.window
    readonly property color header: palette.header
    readonly property color sidebar: palette.sidebar
    readonly property color surface: palette.surface
    readonly property color surfaceAlt: palette.surfaceAlt
    readonly property color surfaceHover: palette.surfaceHover
    readonly property color border: palette.border
    readonly property color divider: palette.divider
    readonly property color text: palette.text
    readonly property color textMuted: palette.textMuted
    readonly property color accent: palette.accent
    readonly property color accentStrong: palette.accentStrong
    readonly property color accentSoft: palette.accentSoft
    readonly property color accentText: palette.accentText
    readonly property color success: palette.success
    readonly property color warning: palette.warning
    readonly property color danger: palette.danger
    readonly property color input: palette.input
    readonly property color overlay: palette.overlay

    function setTheme(themeId) {
        if (themes[themeId] !== undefined)
            current = themeId
    }
}
