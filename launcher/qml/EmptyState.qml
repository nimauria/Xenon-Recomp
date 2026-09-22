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
        id: content
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

        GridLayout {
            id: actionsGrid
            // A host panel can be as narrow as ~270px of usable width (e.g. the
            // Modules page's "Installed modules" sidebar), well below what
            // "Browse Modules" + "Import Local Module" need side by side. A
            // RowLayout has no way to shrink or wrap, so it simply overflowed
            // the card at any width narrower than the buttons' combined natural
            // size. Stack to one column instead, the same way the narrow-panel
            // action rows elsewhere in the launcher already do.
            Layout.alignment: Qt.AlignHCenter
            readonly property bool stacked: root.showPrimary && root.showSecondary
                && (primaryButton.implicitWidth + secondaryButton.implicitWidth + columnSpacing) > content.width
            columns: stacked ? 1 : 2
            columnSpacing: 10
            rowSpacing: 10

            XButton {
                id: primaryButton
                Layout.alignment: Qt.AlignHCenter
                visible: root.showPrimary
                text: root.primaryText
                variant: "primary"
                onClicked: root.primaryClicked()
            }

            XButton {
                id: secondaryButton
                Layout.alignment: Qt.AlignHCenter
                visible: root.showSecondary
                text: root.secondaryText
                onClicked: root.secondaryClicked()
            }
        }
    }
}
