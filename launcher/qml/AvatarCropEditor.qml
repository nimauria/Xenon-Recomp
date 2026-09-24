import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Non-destructive profile picture crop editor. The source image is never
// rewritten: focal point + zoom are persisted and shared with ProfileAvatar.
// Zoom may go below the normal cover scale so logos/badges can be fitted in
// full instead of being forced into an over-cropped portrait treatment.
Popup {
    id: root

    property url imageSource: ""
    property real initialFocalX: 0.5
    property real initialFocalY: 0.5
    property real initialZoom: 1.0
    readonly property real minZoom: 0.55
    readonly property real maxZoom: 3.0
    readonly property real nudgeStep: 0.02

    property real focalX: initialFocalX
    property real focalY: initialFocalY
    property real zoom: initialZoom

    signal applied(real focalX, real focalY, real zoom)

    parent: Overlay.overlay
    width: Math.min(560, parent ? parent.width - Theme.space2Xl * 2 : 560)
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
        zoom = Math.max(minZoom, Math.min(maxZoom, initialZoom))
        viewport.forceActiveFocus()
        NavigationGuard.pushModal()
    }
    onClosed: NavigationGuard.popModal()

    function reset() {
        focalX = 0.5
        focalY = 0.5
        zoom = 1.0
    }

    function fitImage() {
        focalX = 0.5
        focalY = 0.5
        zoom = minZoom
    }

    function nudge(dx, dy) {
        focalX = Math.max(0, Math.min(1, focalX + dx))
        focalY = Math.max(0, Math.min(1, focalY + dy))
    }

    function adjustZoom(delta) {
        zoom = Math.max(minZoom, Math.min(maxZoom, zoom + delta))
    }

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
            text: "Drag the image to reposition it. Zoom out to keep more of the source, or zoom in for a tighter crop. Everything inside the circle becomes your profile picture."
            color: Theme.textMuted
            font.pixelSize: Theme.typeCaption
            wrapMode: Text.WordWrap
        }

        // A visible crop stage rather than an unexplained image viewport.
        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 336
            Layout.preferredHeight: 336
            radius: Theme.panelRadius
            color: Theme.surfaceAlt
            border.width: Theme.borderWidth
            border.color: Theme.divider

            Rectangle {
                id: viewport
                width: 292
                height: 292
                anchors.centerIn: parent
                radius: width / 2
                clip: true
                color: Theme.window
                border.width: Theme.focusWidth
                border.color: activeFocus ? Theme.focusRing : Theme.accent
                focus: true
                activeFocusOnTab: true

                Accessible.role: Accessible.Slider
                Accessible.name: "Profile picture crop position"
                Accessible.description: "Drag or use arrow keys to reposition. Use the slider, wheel, plus or minus to zoom."

                CoverImage {
                    id: cropImage
                    anchors.fill: parent
                    source: root.imageSource
                    fitMode: "cover"
                    focalX: root.focalX
                    focalY: root.focalY
                    zoom: root.zoom
                    decodeHeadroom: 6.0
                }

                // Subtle guides make the crop boundary explicit without
                // obscuring the image.
                Rectangle {
                    anchors.centerIn: parent
                    width: 1
                    height: parent.height
                    color: Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.12)
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width
                    height: 1
                    color: Qt.rgba(Theme.text.r, Theme.text.g, Theme.text.b, 0.12)
                }

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
                        // Moving has an immediate effect whenever the image
                        // extends past the crop on that axis. At fitted sizes
                        // it remains centred because there is nothing to pan.
                        root.nudge(-dx / cropImage.renderedWidth, -dy / cropImage.renderedHeight)
                    }
                    onWheel: (wheel) => {
                        root.adjustZoom(wheel.angleDelta.y > 0 ? 0.1 : -0.1)
                        wheel.accepted = true
                    }
                }
            }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.spaceXs
                text: Math.round(root.zoom * 100) + "%"
                color: Theme.textMuted
                font.pixelSize: Theme.typeCaption
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
                from: root.minZoom
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

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            spacing: Theme.spaceSm

            XButton {
                Layout.fillWidth: true
                text: "Fit image"
                onClicked: root.fitImage()
            }
            XButton {
                Layout.fillWidth: true
                automationId: "avatar-crop-reset"
                text: "Reset crop"
                onClicked: root.reset()
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            Layout.bottomMargin: Theme.spaceLg
            spacing: Theme.spaceSm

            Item { Layout.fillWidth: true }
            XButton {
                automationId: "avatar-crop-cancel"
                text: "Cancel"
                onClicked: root.close()
            }
            XButton {
                automationId: "avatar-crop-apply"
                text: "Apply crop"
                variant: "primary"
                onClicked: {
                    root.applied(root.focalX, root.focalY, root.zoom)
                    root.close()
                }
            }
        }
    }
}
