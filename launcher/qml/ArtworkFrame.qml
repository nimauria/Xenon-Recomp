import QtQuick

Rectangle {
    id: root

    property url source: ""
    property string fallbackTitle: "MODULE ARTWORK"
    property bool hero: false

    radius: 8
    clip: true
    color: Theme.surfaceAlt
    border.width: 1
    border.color: Theme.border

    Image {
        anchors.fill: parent
        source: root.source
        visible: root.source.toString().length > 0
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: true
    }

    Rectangle {
        anchors.fill: parent
        visible: root.source.toString().length === 0
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.surfaceAlt }
            GradientStop { position: 0.65; color: Theme.accentSoft }
            GradientStop { position: 1.0; color: Theme.surface }
        }

        Rectangle {
            anchors.centerIn: parent
            width: root.hero ? 110 : 42
            height: width
            radius: width / 2
            color: Theme.surface
            border.width: 1
            border.color: Theme.accent
            opacity: 0.85

            Text {
                anchors.centerIn: parent
                text: "X"
                color: Theme.accent
                font.pixelSize: root.hero ? 46 : 20
                font.weight: Font.Light
            }
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: root.hero ? 24 : 9
            text: root.fallbackTitle
            color: Theme.textMuted
            font.pixelSize: root.hero ? 13 : 8
            font.letterSpacing: root.hero ? 2 : 1
        }
    }
}
