import QtQuick

Rectangle {
    id: root

    property bool decorated: true
    property real panelOpacity: Theme.highContrast ? 1.0 : Theme.panelOpacity

    radius: Theme.panelRadius
    color: Qt.rgba(Theme.surface.r, Theme.surface.g, Theme.surface.b, root.panelOpacity)
    border.width: Theme.borderWidth
    border.color: Theme.border

    readonly property string frameSource: Theme.decorAsset("panel_frame")

    Image {
        anchors.fill: parent
        source: root.frameSource
        visible: root.decorated && Theme.decorLevel !== "Minimal" && source.toString().length > 0 && root.width >= 420 && root.height >= 150
        fillMode: Image.Stretch
        opacity: 0.28
        smooth: true
    }
}
