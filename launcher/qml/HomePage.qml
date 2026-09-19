import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root

    property var snapshot: ({})
    readonly property var stats: snapshot.stats || ({})
    readonly property var currentSession: snapshot.currentSession || ({})
    readonly property var continueGame: snapshot.continueGame || ({})
    readonly property var recentGames: snapshot.recentGames || []
    readonly property var recentSessions: snapshot.recentSessions || []
    readonly property var recentActivity: snapshot.recentActivity || []
    readonly property bool hasContinueGame: String(continueGame.gameId || "").length > 0
    readonly property bool hasActiveSession: Boolean(currentSession.active)

    function refresh() {
        snapshot = launcherBridge.homeSnapshot()
    }

    function formatDuration(milliseconds) {
        var seconds = Math.max(0, Math.floor(Number(milliseconds || 0) / 1000))
        var hours = Math.floor(seconds / 3600)
        var minutes = Math.floor((seconds % 3600) / 60)
        if (hours > 0) return hours + "h " + minutes + "m"
        if (minutes > 0) return minutes + "m"
        return seconds + "s"
    }

    function sessionTone(item) {
        var outcome = String(item.outcome || "").toLowerCase()
        if (outcome === "failed") return Theme.danger
        if (outcome === "cancelled") return Theme.warning
        return Theme.success
    }

    function activityTone(item) {
        var severity = String(item.severity || "info").toLowerCase()
        if (severity === "error") return Theme.danger
        if (severity === "warning") return Theme.warning
        if (severity === "success") return Theme.success
        return Theme.accent
    }

    function openGame(gameId) {
        launcherBridge.executeCommandPaletteAction("navigate.game", String(gameId || ""), "")
    }

    Connections {
        target: launcherBridge
        function onHomeChanged() { root.refresh() }
    }

    Component.onCompleted: refresh()

    ScrollView {
        anchors.fill: parent
        clip: true

        ColumnLayout {
            width: Math.max(root.width - Theme.spaceSm, 720)
            spacing: Theme.spaceLg

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: "Home"
                        color: Theme.text
                        font.pixelSize: Theme.typeTitle
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: "Recent games, sessions and launcher activity at a glance."
                        color: Theme.textMuted
                        font.pixelSize: Theme.typeBody
                    }
                }

                XButton {
                    text: "Open Library"
                    onClicked: launcherBridge.executeCommandPaletteAction("navigate.library", "", "")
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: width >= 1080 ? 4 : 2
                columnSpacing: Theme.spaceMd
                rowSpacing: Theme.spaceMd

                Repeater {
                    model: [
                        { label: "Games", value: String(root.stats.games || 0), detail: "in Library" },
                        { label: "Play time", value: root.formatDuration(root.stats.totalPlayTimeMs || 0), detail: String(root.stats.totalPlayCount || 0) + " launches" },
                        { label: "Sessions", value: String(root.stats.sessionCount || 0), detail: String(root.stats.failedSessions || 0) + " failed" },
                        { label: "Modules", value: String(root.stats.enabledModules || 0) + " / " + String(root.stats.modules || 0), detail: String(root.stats.moduleUpdates || 0) + " updates available" }
                    ]
                    delegate: XPanel {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredHeight: 108

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.spaceMd
                            spacing: 3
                            Text { text: modelData.label; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                            Text { text: modelData.value; color: Theme.text; font.pixelSize: Theme.typeTitle; font.weight: Font.DemiBold }
                            Text { text: modelData.detail; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                        }
                    }
                }
            }

            XPanel {
                Layout.fillWidth: true
                visible: root.hasContinueGame || root.hasActiveSession
                implicitHeight: continueContent.implicitHeight + Theme.spaceLg * 2

                RowLayout {
                    id: continueContent
                    anchors.fill: parent
                    anchors.margins: Theme.spaceLg
                    spacing: Theme.spaceLg

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        RowLayout {
                            Layout.fillWidth: true
                            Text {
                                text: root.hasActiveSession ? "Current session" : "Continue"
                                color: Theme.textMuted
                                font.pixelSize: Theme.typeCaption
                                font.weight: Font.DemiBold
                            }
                            StatusPill {
                                visible: root.hasActiveSession
                                label: String(root.currentSession.stateLabel || root.currentSession.state || "SESSION").toUpperCase()
                                tone: String(root.currentSession.state || "") === "failed" ? Theme.danger : Theme.accent
                            }
                        }
                        Text {
                            text: root.hasActiveSession
                                  ? String(root.currentSession.title || root.continueGame.title || "Current game")
                                  : String(root.continueGame.title || "Recent game")
                            color: Theme.text
                            font.pixelSize: Theme.typeSubtitle
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Text {
                            text: root.hasActiveSession
                                  ? String(root.currentSession.moduleName || root.continueGame.moduleName || "Xenon session")
                                    + " • " + root.formatDuration(root.currentSession.elapsedMs || 0)
                                  : String(root.continueGame.moduleName || "Xenon module")
                                    + " • " + String(root.continueGame.lastPlayedLabel || "Recently played")
                            color: Theme.textMuted
                            font.pixelSize: Theme.typeBody
                            elide: Text.ElideRight
                        }
                    }

                    XButton {
                        text: root.hasActiveSession && String(root.currentSession.state || "") === "running" ? "Open Game" : "Open"
                        variant: "primary"
                        onClicked: root.openGame(root.hasActiveSession ? root.currentSession.gameId : root.continueGame.gameId)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.spaceMd

                XPanel {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 3
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: recentGamesColumn.implicitHeight + Theme.spaceLg * 2

                    ColumnLayout {
                        id: recentGamesColumn
                        anchors.fill: parent
                        anchors.margins: Theme.spaceLg
                        spacing: Theme.spaceSm

                        RowLayout {
                            Layout.fillWidth: true
                            Text { Layout.fillWidth: true; text: "Recently played"; color: Theme.text; font.pixelSize: Theme.typeSubtitle; font.weight: Font.DemiBold }
                            XButton { text: "Library"; variant: "ghost"; onClicked: launcherBridge.executeCommandPaletteAction("navigate.library", "", "") }
                        }

                        Text {
                            visible: root.recentGames.length === 0
                            Layout.fillWidth: true
                            text: "Games you launch through Xenon will appear here."
                            color: Theme.textMuted
                            font.pixelSize: Theme.typeBody
                            wrapMode: Text.WordWrap
                        }

                        Repeater {
                            model: root.recentGames
                            delegate: Rectangle {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.preferredHeight: 58
                                radius: Theme.controlRadius
                                color: gameHover.hovered ? Theme.surfaceHover : "transparent"
                                border.width: Theme.borderWidth
                                border.color: Theme.divider

                                HoverHandler { id: gameHover }
                                TapHandler { onTapped: root.openGame(modelData.gameId) }

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: Theme.spaceSm
                                    spacing: Theme.spaceSm
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1
                                        Text { Layout.fillWidth: true; text: String(modelData.title || "Game"); color: Theme.text; font.pixelSize: Theme.typeBody; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                        Text { Layout.fillWidth: true; text: String(modelData.moduleName || "Xenon module"); color: Theme.textMuted; font.pixelSize: Theme.typeCaption; elide: Text.ElideRight }
                                    }
                                    ColumnLayout {
                                        Layout.alignment: Qt.AlignRight
                                        spacing: 1
                                        Text { text: String(modelData.lastPlayedLabel || ""); color: Theme.textMuted; font.pixelSize: Theme.typeCaption; horizontalAlignment: Text.AlignRight }
                                        Text { text: root.formatDuration(modelData.totalPlayTimeMs || 0); color: Theme.textMuted; font.pixelSize: Theme.typeCaption; horizontalAlignment: Text.AlignRight }
                                    }
                                }
                            }
                        }
                    }
                }

                XPanel {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 2
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: sessionColumn.implicitHeight + Theme.spaceLg * 2

                    ColumnLayout {
                        id: sessionColumn
                        anchors.fill: parent
                        anchors.margins: Theme.spaceLg
                        spacing: Theme.spaceSm

                        Text { text: "Recent sessions"; color: Theme.text; font.pixelSize: Theme.typeSubtitle; font.weight: Font.DemiBold }
                        Text {
                            visible: root.recentSessions.length === 0
                            Layout.fillWidth: true
                            text: "Session outcomes will be recorded here once games are launched."
                            color: Theme.textMuted
                            font.pixelSize: Theme.typeBody
                            wrapMode: Text.WordWrap
                        }
                        Repeater {
                            model: root.recentSessions
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm

                                Rectangle { width: 4; Layout.preferredHeight: 34; radius: 2; color: root.sessionTone(modelData) }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text { Layout.fillWidth: true; text: String(modelData.title || "Game session"); color: Theme.text; font.pixelSize: Theme.typeBody; elide: Text.ElideRight }
                                    Text { Layout.fillWidth: true; text: String(modelData.outcomeLabel || "Session") + " • " + root.formatDuration(modelData.elapsedMs || 0); color: Theme.textMuted; font.pixelSize: Theme.typeCaption; elide: Text.ElideRight }
                                }
                                Text { text: String(modelData.whenLabel || ""); color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                spacing: Theme.spaceMd

                XPanel {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 3
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: activityColumn.implicitHeight + Theme.spaceLg * 2

                    ColumnLayout {
                        id: activityColumn
                        anchors.fill: parent
                        anchors.margins: Theme.spaceLg
                        spacing: Theme.spaceSm

                        RowLayout {
                            Layout.fillWidth: true
                            Text { Layout.fillWidth: true; text: "Recent activity"; color: Theme.text; font.pixelSize: Theme.typeSubtitle; font.weight: Font.DemiBold }
                            Text {
                                visible: Number(root.stats.unreadNotifications || 0) > 0
                                text: String(root.stats.unreadNotifications) + " unread"
                                color: Theme.accent
                                font.pixelSize: Theme.typeCaption
                            }
                        }

                        Text {
                            visible: root.recentActivity.length === 0
                            Layout.fillWidth: true
                            text: "Updates, imports, module changes and session errors will appear here."
                            color: Theme.textMuted
                            font.pixelSize: Theme.typeBody
                            wrapMode: Text.WordWrap
                        }

                        Repeater {
                            model: root.recentActivity
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm

                                Rectangle { width: 8; height: 8; radius: 4; color: root.activityTone(modelData) }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    Text { Layout.fillWidth: true; text: String(modelData.title || "Xenon Launcher"); color: modelData.read ? Theme.textMuted : Theme.text; font.pixelSize: Theme.typeBody; font.weight: modelData.read ? Font.Normal : Font.DemiBold; elide: Text.ElideRight }
                                    Text { Layout.fillWidth: true; text: String(modelData.message || ""); color: Theme.textMuted; font.pixelSize: Theme.typeCaption; elide: Text.ElideRight }
                                }
                                Text { text: String(modelData.whenLabel || ""); color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                                XButton {
                                    visible: String(modelData.commandId || "").length > 0
                                    text: String(modelData.actionLabel || "Open")
                                    variant: "ghost"
                                    onClicked: launcherBridge.executeNotificationAction(String(modelData.id || ""))
                                }
                            }
                        }
                    }
                }

                XPanel {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 2
                    Layout.alignment: Qt.AlignTop
                    implicitHeight: quickColumn.implicitHeight + Theme.spaceLg * 2

                    ColumnLayout {
                        id: quickColumn
                        anchors.fill: parent
                        anchors.margins: Theme.spaceLg
                        spacing: Theme.spaceSm
                        Text { text: "Quick actions"; color: Theme.text; font.pixelSize: Theme.typeSubtitle; font.weight: Font.DemiBold }
                        XButton { Layout.fillWidth: true; text: "Browse Module Catalog"; onClicked: launcherBridge.executeCommandPaletteAction("modules.catalog", "", "catalog") }
                        XButton { Layout.fillWidth: true; text: "Check Module Updates"; onClicked: launcherBridge.executeCommandPaletteAction("updates.modules.check", "", "") }
                        XButton { Layout.fillWidth: true; text: "Create Support Bundle"; onClicked: launcherBridge.executeCommandPaletteAction("support.bundle", "", "") }
                        XButton { Layout.fillWidth: true; text: "Settings"; variant: "ghost"; onClicked: launcherBridge.executeCommandPaletteAction("navigate.settings", "", "") }
                    }
                }
            }

            Item { Layout.preferredHeight: Theme.spaceLg }
        }
    }
}
