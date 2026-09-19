import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root

    property var allEntries: []
    property bool unreadOnly: false

    width: Math.min(440, parent && parent.Window.window ? parent.Window.window.width - 48 : 440)
    height: Math.min(590, parent && parent.Window.window ? parent.Window.window.height - 120 : 590)
    padding: 0
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

    function refresh() {
        allEntries = launcherBridge.notifications()
    }

    function openPanel() {
        refresh()
        if (!visible)
            open()
    }

    function togglePanel() {
        if (visible)
            close()
        else
            openPanel()
    }

    function visibleEntries() {
        if (!unreadOnly)
            return allEntries
        var filtered = []
        for (var i = 0; i < allEntries.length; ++i) {
            if (!allEntries[i].read)
                filtered.push(allEntries[i])
        }
        return filtered
    }

    function severityColor(severity) {
        switch (severity) {
        case "success": return Theme.success
        case "warning": return Theme.warning
        case "error": return Theme.danger
        default: return Theme.accent
        }
    }

    function severityLabel(severity) {
        switch (severity) {
        case "success": return "SUCCESS"
        case "warning": return "WARNING"
        case "error": return "ERROR"
        default: return "INFO"
        }
    }

    function timestampLabel(value) {
        var date = new Date(value)
        if (isNaN(date.getTime()))
            return ""
        var now = new Date()
        var delta = Math.max(0, now.getTime() - date.getTime())
        if (delta < 60000)
            return "Just now"
        if (delta < 3600000)
            return Math.floor(delta / 60000) + "m ago"
        if (delta < 86400000)
            return Math.floor(delta / 3600000) + "h ago"
        return Qt.formatDateTime(date, "dd MMM  hh:mm")
    }

    onOpened: refresh()

    Connections {
        target: launcherBridge
        function onNotificationsChanged() { root.refresh() }
    }

    background: Rectangle {
        radius: Theme.panelRadius
        color: Theme.surfaceRaised
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    contentItem: ColumnLayout {
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 68

            Column {
                anchors.left: parent.left
                anchors.leftMargin: Theme.spaceLg
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2

                Text {
                    text: "Notifications"
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }
                Text {
                    text: launcherBridge.notificationUnreadCount > 0
                          ? launcherBridge.notificationUnreadCount + " unread"
                          : "You're all caught up"
                    color: Theme.textMuted
                    font.pixelSize: Theme.typeCaption
                }
            }

            Row {
                anchors.right: parent.right
                anchors.rightMargin: Theme.spaceMd
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spaceXs

                XButton {
                    text: "Mark all read"
                    variant: "ghost"
                    enabled: launcherBridge.notificationUnreadCount > 0
                    onClicked: launcherBridge.markAllNotificationsRead()
                }
                XIconButton {
                    iconName: "close"
                    tooltip: "Close notifications"
                    variant: "ghost"
                    onClicked: root.close()
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceMd
            Layout.rightMargin: Theme.spaceMd
            Layout.topMargin: Theme.spaceSm
            Layout.bottomMargin: Theme.spaceSm
            spacing: Theme.spaceXs

            XButton {
                text: "All"
                variant: root.unreadOnly ? "ghost" : "secondary"
                onClicked: root.unreadOnly = false
            }
            XButton {
                text: "Unread"
                variant: root.unreadOnly ? "secondary" : "ghost"
                onClicked: root.unreadOnly = true
            }
            Item { Layout.fillWidth: true }
            XButton {
                text: "Clear"
                variant: "ghost"
                enabled: root.allEntries.length > 0
                onClicked: launcherBridge.clearNotifications()
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: Theme.spaceSm
                clip: true
                spacing: Theme.spaceSm
                model: root.visibleEntries()
                boundsBehavior: Flickable.StopAtBounds

                delegate: Rectangle {
                    id: card
                    required property var modelData
                    width: list.width
                    height: cardContent.implicitHeight + Theme.spaceLg
                    radius: Theme.controlRadius
                    color: modelData.read ? Theme.surface : Theme.surfaceAlt
                    border.width: Theme.borderWidth
                    border.color: modelData.read ? Theme.border : root.severityColor(modelData.severity)

                    MouseArea {
                        anchors.fill: parent
                        acceptedButtons: Qt.LeftButton
                        onClicked: {
                            launcherBridge.markNotificationRead(card.modelData.id, true)
                            if (card.modelData.commandId && card.modelData.commandId.length > 0) {
                                launcherBridge.executeNotificationAction(card.modelData.id)
                                root.close()
                            }
                        }
                    }

                    RowLayout {
                        id: cardContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: Theme.spaceMd
                        spacing: Theme.spaceSm

                        Rectangle {
                            Layout.preferredWidth: 4
                            Layout.fillHeight: true
                            Layout.minimumHeight: 62
                            radius: 2
                            color: root.severityColor(card.modelData.severity)
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceXs

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceXs

                                Text {
                                    Layout.fillWidth: true
                                    text: card.modelData.title
                                    color: Theme.text
                                    font.pixelSize: Theme.typeBody
                                    font.weight: card.modelData.read ? Font.Medium : Font.DemiBold
                                    elide: Text.ElideRight
                                }

                                Text {
                                    text: root.timestampLabel(card.modelData.timestamp)
                                    color: Theme.textMuted
                                    font.pixelSize: Math.max(10, Theme.typeCaption * 0.9)
                                }

                                XIconButton {
                                    Layout.preferredWidth: 28
                                    Layout.preferredHeight: 28
                                    iconName: "close"
                                    tooltip: "Dismiss"
                                    variant: "ghost"
                                    onClicked: launcherBridge.dismissNotification(card.modelData.id)
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                text: card.modelData.message
                                color: Theme.textMuted
                                font.pixelSize: Theme.typeCaption
                                wrapMode: Text.WordWrap
                                maximumLineCount: 4
                                elide: Text.ElideRight
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm

                                Text {
                                    text: root.severityLabel(card.modelData.severity)
                                    color: root.severityColor(card.modelData.severity)
                                    font.pixelSize: Math.max(9, Theme.typeCaption * 0.82)
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    text: String(card.modelData.source || "launcher").toUpperCase()
                                    color: Theme.textMuted
                                    font.pixelSize: Math.max(9, Theme.typeCaption * 0.82)
                                }
                                Text {
                                    visible: Number(card.modelData.repeatCount || 1) > 1
                                    text: "×" + card.modelData.repeatCount
                                    color: Theme.textMuted
                                    font.pixelSize: Math.max(9, Theme.typeCaption * 0.82)
                                }
                                Item { Layout.fillWidth: true }

                                XButton {
                                    visible: card.modelData.commandId && card.modelData.commandId.length > 0
                                    text: card.modelData.actionLabel && card.modelData.actionLabel.length > 0
                                          ? card.modelData.actionLabel : "Open"
                                    variant: "ghost"
                                    onClicked: {
                                        launcherBridge.executeNotificationAction(card.modelData.id)
                                        root.close()
                                    }
                                }
                            }
                        }
                    }
                }

                ScrollBar.vertical: ScrollBar { }
            }

            Column {
                anchors.centerIn: parent
                width: Math.min(parent.width - Theme.spaceXl * 2, 310)
                spacing: Theme.spaceSm
                visible: root.visibleEntries().length === 0

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: root.unreadOnly ? "No unread notifications" : "No notifications yet"
                    color: Theme.text
                    font.pixelSize: Theme.typeBody
                    font.weight: Font.DemiBold
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.unreadOnly
                          ? "New launcher events will appear here when they need your attention."
                          : "Updates, session failures, imports and other launcher events will be kept here."
                    color: Theme.textMuted
                    font.pixelSize: Theme.typeCaption
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
