import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Interactive crop/position editor for a profile picture. Operates purely on
// metadata (focal point + zoom) over the untouched source image - see
// ProfileService::importAvatar(), which copies the original file as-is and
// never bakes a crop into it - so Reset always returns to a knowable state
// and re-opening this editor later can start from whatever was last saved.
Popup {
    id: root

    property url imageSource: ""
    property real initialFocalX: 0.5
    property real initialFocalY: 0.5
    property real initialZoom: 1.0
    readonly property real maxZoom: 3.0
    readonly property real nudgeStep: 0.02

    property real focalX: initialFocalX
    property real focalY: initialFocalY
    property real zoom: initialZoom

    signal applied(real focalX, real focalY, real zoom)

    parent: Overlay.overlay
    width: Math.min(420, parent ? parent.width - Theme.space2Xl * 2 : 420)
    implicitHeight: contentColumn.implicitHeight + Theme.spaceXl * 2
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    modal: true
    focus: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: {
        focalX = initialFocalX
        focalY = initialFocalY
        zoom = initialZoom
        viewport.forceActiveFocus()
        NavigationGuard.pushModal()
    }
    onClosed: NavigationGuard.popModal()

    function reset() {
        focalX = 0.5
        focalY = 0.5
        zoom = 1.0
    }

    function nudge(dx, dy) {
        focalX = Math.max(0, Math.min(1, focalX + dx))
        focalY = Math.max(0, Math.min(1, focalY + dy))
    }

    function adjustZoom(delta) {
        zoom = Math.max(1.0, Math.min(root.maxZoom, zoom + delta))
    }

    // Controller support (Part 20): D-pad/stick reposition (the same
    // navigateUp/Down/Left/Right the rest of the launcher would otherwise
    // use to move focus - suppressed here via NavigationGuard so this
    // dialog gets them instead), triggers zoom (the same physical inputs
    // "page/scroll larger content" uses elsewhere - zooming a crop is this
    // dialog's equivalent of paging), A applies, B cancels, X (the
    // launcher's reserved "secondary action" button) resets.
    Connections {
        target: launcherBridge
        enabled: root.visible
        function onFrontendAction(action) {
            switch (action) {
            case "up": root.nudge(0, -root.nudgeStep); break
            case "down": root.nudge(0, root.nudgeStep); break
            case "left": root.nudge(-root.nudgeStep, 0); break
            case "right": root.nudge(root.nudgeStep, 0); break
            case "scrollUp": root.adjustZoom(0.15); break
            case "scrollDown": root.adjustZoom(-0.15); break
            case "confirm":
                root.applied(root.focalX, root.focalY, root.zoom)
                root.close()
                break
            case "cancel": case "back":
                root.close()
                break
            case "secondary":
                root.reset()
                break
            }
        }
    }

    Overlay.modal: Rectangle { color: Theme.overlay }

    background: Rectangle {
        color: Theme.surfaceRaised
        radius: Theme.panelRadius
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        id: contentColumn
        spacing: Theme.spaceLg

        Item { Layout.preferredHeight: Theme.spaceXs }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            text: "Adjust profile picture"
            color: Theme.text
            font.pixelSize: Theme.typeSubtitle
            font.weight: Font.DemiBold
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            text: "Drag to reposition, scroll or use the slider to zoom. Arrow keys nudge position; +/- zoom."
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
            wrapMode: Text.WordWrap
        }

        // The crop viewport: WYSIWYG for exactly what ProfileAvatar.qml will
        // render, since both go through the same CoverImage component with
        // the same focalX/focalY/zoom values.
        Item {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 220
            Layout.preferredHeight: 220

            Rectangle {
                id: viewport
                anchors.fill: parent
                radius: width / 2
                clip: true
                color: Theme.surfaceAlt
                border.width: Theme.focusWidth
                border.color: activeFocus ? Theme.focusRing : Theme.border
                focus: true
                activeFocusOnTab: true

                Accessible.role: Accessible.Slider
                Accessible.name: "Profile picture crop position"
                Accessible.description: "Drag, or use arrow keys and +/- to reposition and zoom the profile picture."

                CoverImage {
                    id: cropImage
                    anchors.fill: parent
                    source: root.imageSource
                    fitMode: "cover"
                    focalX: root.focalX
                    focalY: root.focalY
                    zoom: root.zoom
                    // Keep one sufficiently detailed texture throughout the crop
                    // session; zooming must remain a transform-only operation.
                    decodeHeadroom: 6.0
                }

                // Claims these keys before Qt's shortcut system can - without
                // this, Main.qml's global Up/Down/Left/Right/PageUp/PageDown
                // Shortcut items (needed for keyboard-only launcher
                // navigation elsewhere) would intercept the key event before
                // it ever reaches Keys.onPressed below, silently breaking
                // keyboard crop control while this dialog is open.
                Keys.onShortcutOverride: (event) => {
                    switch (event.key) {
                    case Qt.Key_Left: case Qt.Key_Right: case Qt.Key_Up: case Qt.Key_Down:
                    case Qt.Key_Plus: case Qt.Key_Equal: case Qt.Key_Minus:
                    case Qt.Key_PageUp: case Qt.Key_PageDown: case Qt.Key_Home:
                        event.accepted = true
                    }
                }

                Keys.onPressed: (event) => {
                    switch (event.key) {
                    case Qt.Key_Left: root.nudge(-root.nudgeStep, 0); event.accepted = true; break
                    case Qt.Key_Right: root.nudge(root.nudgeStep, 0); event.accepted = true; break
                    case Qt.Key_Up: root.nudge(0, -root.nudgeStep); event.accepted = true; break
                    case Qt.Key_Down: root.nudge(0, root.nudgeStep); event.accepted = true; break
                    case Qt.Key_Plus:
                    case Qt.Key_Equal:
                    case Qt.Key_PageUp:
                        root.adjustZoom(0.1); event.accepted = true; break
                    case Qt.Key_Minus:
                    case Qt.Key_PageDown:
                        root.adjustZoom(-0.1); event.accepted = true; break
                    case Qt.Key_Home:
                        root.reset(); event.accepted = true; break
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.SizeAllCursor
                    property real lastX: 0
                    property real lastY: 0
                    onPressed: (mouse) => {
                        viewport.forceActiveFocus()
                        lastX = mouse.x
                        lastY = mouse.y
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed || cropImage.renderedWidth <= 0 || cropImage.renderedHeight <= 0) return
                        const dx = mouse.x - lastX
                        const dy = mouse.y - lastY
                        lastX = mouse.x
                        lastY = mouse.y
                        root.nudge(-dx / cropImage.renderedWidth, -dy / cropImage.renderedHeight)
                    }
                    onWheel: (wheel) => {
                        root.adjustZoom(wheel.angleDelta.y > 0 ? 0.1 : -0.1)
                        wheel.accepted = true
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            spacing: Theme.spaceSm

            XIconButton {
                automationId: "avatar-crop-zoom-out"
                glyph: "−"
                tooltip: "Zoom out"
                onClicked: root.adjustZoom(-0.1)
            }

            XSlider {
                id: zoomSlider
                Layout.fillWidth: true
                from: 1.0
                to: root.maxZoom
                value: root.zoom
                accessibleName: "Zoom"
                onMoved: root.zoom = value
            }

            XIconButton {
                automationId: "avatar-crop-zoom-in"
                glyph: "+"
                tooltip: "Zoom in"
                onClicked: root.adjustZoom(0.1)
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            Layout.bottomMargin: Theme.spaceLg
            spacing: Theme.spaceSm

            XButton {
                automationId: "avatar-crop-reset"
                text: "Reset"
                onClicked: root.reset()
            }
            Item { Layout.fillWidth: true }
            XButton {
                automationId: "avatar-crop-cancel"
                text: "Cancel"
                onClicked: root.close()
            }
            XButton {
                automationId: "avatar-crop-apply"
                text: "Apply"
                variant: "primary"
                onClicked: {
                    root.applied(root.focalX, root.focalY, root.zoom)
                    root.close()
                }
            }
        }
    }
}
