import QtQuick

Rectangle {
    id: root

    property url source: ""
    property string fallbackTitle: ""
    property bool hero: false
    // Normalized [0,1] source-image point that must stay visible under the
    // cover crop (e.g. supplied by a module's tileArtFocal/heroArtFocal
    // manifest metadata). Defaults to center, matching prior behavior for
    // any artwork that declares no focal point.
    property real focalX: 0.5
    property real focalY: 0.5

    radius: Theme.controlRadius
    clip: true
    color: Theme.surfaceAlt
    border.width: Theme.borderWidth
    border.color: Theme.border

    CoverImage {
        anchors.fill: parent
        source: root.source
        visible: root.source.toString().length > 0
        fitMode: "cover"
        focalX: root.focalX
        focalY: root.focalY
    }

    Rectangle {
        anchors.fill: parent
        visible: root.source.toString().length === 0
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.surfaceAlt }
            GradientStop { position: 0.65; color: Theme.accentSoft }
            GradientStop { position: 1.0; color: Theme.surface }
        }

        XenonBrand {
            anchors.centerIn: parent
            width: root.hero ? Math.min(100, parent.width * 0.15) : Math.min(44, parent.width * 0.44)
            height: width
            asset: "mark"
            brandColor: Theme.accent
            opacity: 0.88
        }

        Text {
            visible: root.fallbackTitle.length > 0
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: root.hero ? Theme.spaceLg : Theme.spaceSm
            width: parent.width - Theme.spaceLg * 2
            text: root.fallbackTitle
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }
    }
}
