import QtQuick
import QtQuick.Layouts

Item {
    id: root

    property string glyph: "＋"
    property string title: "Nothing here yet"
    property string description: ""
    property string primaryText: ""
    property string secondaryText: ""
    property bool showPrimary: primaryText.length > 0
    property bool showSecondary: secondaryText.length > 0

    signal primaryClicked()
    signal secondaryClicked()

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 560)
        spacing: 14

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 76
            Layout.preferredHeight: 76
            radius: 38
            color: Theme.accentSoft
            border.width: 1
            border.color: Theme.accent

            Text {
                anchors.centerIn: parent
                text: root.glyph
                color: Theme.accent
                font.pixelSize: 32
                font.weight: Font.Light
            }
        }

        Text {
            Layout.fillWidth: true
            text: root.title
            color: Theme.text
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            font.pixelSize: 24
            font.weight: Font.DemiBold
        }

        Text {
            Layout.fillWidth: true
            text: root.description
            visible: text.length > 0
            color: Theme.textMuted
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: 1.25
            font.pixelSize: 12
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 10

            XButton {
                visible: root.showPrimary
                text: root.primaryText
                variant: "primary"
                onClicked: root.primaryClicked()
            }

            XButton {
                visible: root.showSecondary
                text: root.secondaryText
                onClicked: root.secondaryClicked()
            }
        }
    }
}
