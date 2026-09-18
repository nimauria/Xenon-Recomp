import QtQuick

Rectangle {
    id: root

    property bool decorated: false
    property real panelOpacity: Theme.highContrast ? 1.0 : 0.94

    radius: Theme.panelRadius
    color: Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, root.panelOpacity)
    border.width: Theme.borderWidth
    border.color: Theme.border

    readonly property string frameSource: Theme.effectiveThemeId === "industrial"
        ? "qrc:/theme-art/decor/amber/panel_frame_amber.svg"
        : Theme.effectiveThemeId === "carbon"
          ? "qrc:/theme-art/decor/green/panel_frame_green.svg"
          : Theme.effectiveThemeId === "xenon-dark"
            ? "qrc:/theme-art/decor/blue/panel_frame_blue.svg" : ""

    Image {
        anchors.fill: parent
        source: root.frameSource
        visible: root.decorated && source.toString().length > 0 && root.width >= 420 && root.height >= 150
        fillMode: Image.Stretch
        opacity: 0.28
        smooth: true
    }
}
