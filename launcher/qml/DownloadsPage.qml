import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Unified user-facing background activity centre. The backend snapshot composes
// the real launcher updater, module updater and game-preparation session into a
// single job vocabulary, so this page does not duplicate updater business
// logic or create fake jobs.
Item {
    id: root

    property int revision: 0
    property int rateRevision: 0
    property string viewMode: launcherBridge.stringSetting("downloads/viewMode", "Overview")
    property var transferRates: ({})
    property var transferSamples: ({})

    readonly property var snapshot: {
        var r = root.revision
        return launcherBridge.downloadActivitySnapshot()
    }
    readonly property var jobs: snapshot.jobs || []
    readonly property var historyEntries: snapshot.history || []
    readonly property var activeJobs: jobs.filter(function(job) { return Boolean(job.active) })
    readonly property var readyJobs: jobs.filter(function(job) { return Boolean(job.ready) })
    readonly property var failedJobs: jobs.filter(function(job) { return Boolean(job.failed) })
    readonly property int activeCount: Number(snapshot.activeCount || 0)
    readonly property int readyCount: Number(snapshot.readyCount || 0)
    readonly property int failureCount: Number(snapshot.failureCount || 0)
    readonly property bool showActive: viewMode === "Overview" || viewMode === "Active"
    readonly property bool showReady: viewMode === "Overview" || viewMode === "Ready"
    readonly property bool showIssues: viewMode === "Overview" || viewMode === "Issues"
    readonly property bool showHistory: viewMode === "Overview" || viewMode === "History"

    signal requestPage(int index)

    function setViewMode(value) {
        root.viewMode = value
        launcherBridge.setSettingValue("downloads/viewMode", value)
    }

    function statusLabel(status) {
        if (status === "checking") return "Checking"
        if (status === "downloading") return "Downloading"
        if (status === "installing") return "Installing"
        if (status === "ready-to-install") return "Ready to install"
        if (status === "update-available") return "Update available"
        if (status === "rolling-back") return "Rolling back"
        if (status === "preparing") return "Preparing"
        if (status === "validating") return "Validating"
        if (status === "starting") return "Starting"
        if (status === "stopping") return "Stopping"
        if (status === "error" || status === "failed") return "Needs attention"
        if (status === "up-to-date") return "Up to date"
        if (status === "idle") return "Idle"
        return status.length > 0 ? status : "Idle"
    }

    function formatBytes(value) {
        var bytes = Number(value || 0)
        if (bytes <= 0) return "0 B"
        if (bytes < 1024) return Math.round(bytes) + " B"
        if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + " KiB"
        if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MiB"
        return (bytes / (1024 * 1024 * 1024)).toFixed(2) + " GiB"
    }

    function formatRate(value) {
        var rate = Number(value || 0)
        if (rate <= 0) return ""
        return root.formatBytes(rate) + "/s"
    }

    function formatEta(seconds) {
        var value = Math.max(0, Math.round(Number(seconds || 0)))
        if (value <= 0) return ""
        if (value < 60) return value + "s left"
        if (value < 3600) return Math.floor(value / 60) + "m " + (value % 60) + "s left"
        var hours = Math.floor(value / 3600)
        var minutes = Math.floor((value % 3600) / 60)
        return hours + "h " + minutes + "m left"
    }

    function rateFor(jobId) {
        var r = root.rateRevision
        return root.transferRates[String(jobId || "")] || ({ bytesPerSecond: 0, etaSeconds: 0 })
    }

    function progressText(job) {
        var status = root.statusLabel(String(job.status || ""))
        var total = Number(job.downloadTotalBytes || 0)
        var done = Number(job.downloadedBytes || 0)
        var parts = [status]
        if (total > 0) parts.push(root.formatBytes(done) + " of " + root.formatBytes(total))
        var rate = root.rateFor(job.id)
        if (Number(rate.bytesPerSecond || 0) > 0) parts.push(root.formatRate(rate.bytesPerSecond))
        if (Number(rate.etaSeconds || 0) > 0) parts.push(root.formatEta(rate.etaSeconds))
        return parts.join(" • ")
    }

    function primaryAction(job) {
        if (Boolean(job.failed) || Boolean(job.canRetry)) return "retry"
        if (Boolean(job.canInstall)) return "install"
        if (Boolean(job.canDownload)) return "download"
        if (Boolean(job.canCheck)) return "check"
        return ""
    }

    function primaryActionLabel(job) {
        var action = root.primaryAction(job)
        if (action === "install") return "Install"
        if (action === "download") return "Download"
        if (action === "retry") return "Retry"
        if (action === "check") return "Check again"
        return ""
    }

    function runJobAction(job, action) {
        if (!job || String(job.id || "").length === 0 || String(action || "").length === 0) return
        launcherBridge.executeDownloadActivityAction(String(job.id), String(action))
    }

    function checkAll() {
        launcherBridge.requestLauncherUpdateCheck()
        launcherBridge.requestAllModuleUpdateChecks()
    }

    function retryAllFailed() {
        // Snapshot jobs already encode whether Retry is valid. Reuse the same
        // semantic action path as each row instead of special-casing updater
        // implementations in QML.
        for (var i = 0; i < root.failedJobs.length; ++i) {
            var job = root.failedJobs[i]
            if (Boolean(job.canRetry) || Boolean(job.failed))
                root.runJobAction(job, "retry")
        }
    }

    function refreshTransferRates() {
        var now = Date.now()
        var nextSamples = Object.assign({}, root.transferSamples)
        var nextRates = Object.assign({}, root.transferRates)
        var activeIds = ({})

        for (var i = 0; i < root.activeJobs.length; ++i) {
            var job = root.activeJobs[i]
            var id = String(job.id || "")
            var done = Number(job.downloadedBytes || 0)
            var total = Number(job.downloadTotalBytes || 0)
            if (id.length === 0 || total <= 0) continue
            activeIds[id] = true
            var previous = nextSamples[id]
            if (previous && now > Number(previous.time || 0) && done >= Number(previous.bytes || 0)) {
                var elapsed = (now - Number(previous.time || 0)) / 1000.0
                var instantaneous = elapsed > 0 ? (done - Number(previous.bytes || 0)) / elapsed : 0
                var oldRate = Number((nextRates[id] || {}).bytesPerSecond || 0)
                var smoothed = instantaneous > 0
                    ? (oldRate > 0 ? oldRate * 0.65 + instantaneous * 0.35 : instantaneous) : oldRate * 0.8
                var eta = smoothed > 1 ? Math.max(0, total - done) / smoothed : 0
                nextRates[id] = ({ bytesPerSecond: smoothed, etaSeconds: eta })
            }
            nextSamples[id] = ({ bytes: done, time: now })
        }

        // Drop stale samples so a later transfer does not inherit an old rate.
        for (var sampleId in nextSamples) {
            if (!activeIds[sampleId]) {
                delete nextSamples[sampleId]
                delete nextRates[sampleId]
            }
        }
        root.transferSamples = nextSamples
        root.transferRates = nextRates
        root.rateRevision += 1
    }

    function handleDirectionalNavigation(direction) {
        // Controls participate in the launcher's shared focus chain. Keeping
        // this false avoids a second, Downloads-specific focus system.
        return false
    }

    Shortcut {
        sequence: "Ctrl+R"
        enabled: root.visible
        onActivated: root.checkAll()
    }

    Connections {
        target: launcherBridge
        function onDownloadActivityChanged() { activityRefreshThrottle.restart() }
        function onSettingChanged(key, value) {
            if (key === "downloads/viewMode")
                root.viewMode = launcherBridge.stringSetting(key, "Overview")
        }
    }

    // Network progress signals can be very frequent. Coalescing them keeps
    // QML model rebuilding bounded while remaining visually smooth.
    Timer {
        id: activityRefreshThrottle
        interval: 90
        repeat: false
        onTriggered: root.revision += 1
    }

    Timer {
        interval: 500
        running: root.activeCount > 0
        repeat: true
        onTriggered: root.refreshTransferRates()
    }

    XSettingsPage {
        id: page
        anchors.fill: parent
        title: "Downloads"
        description: "Updates, downloads, preparation and background activity managed by Xenon."

        Flow {
            Layout.fillWidth: true
            spacing: Theme.spaceSm

            XButton {
                text: "Overview"
                variant: root.viewMode === "Overview" ? "primary" : "ghost"
                onClicked: root.setViewMode("Overview")
            }
            XButton {
                text: "Active  " + root.activeCount
                variant: root.viewMode === "Active" ? "primary" : "ghost"
                onClicked: root.setViewMode("Active")
            }
            XButton {
                text: "Ready  " + root.readyCount
                variant: root.viewMode === "Ready" ? "primary" : "ghost"
                onClicked: root.setViewMode("Ready")
            }
            XButton {
                text: "Issues  " + root.failureCount
                variant: root.viewMode === "Issues" ? "primary" : "ghost"
                onClicked: root.setViewMode("Issues")
            }
            XButton {
                text: "History  " + root.historyEntries.length
                variant: root.viewMode === "History" ? "primary" : "ghost"
                onClicked: root.setViewMode("History")
            }
        }

        XPanel {
            visible: root.viewMode === "Overview"
            Layout.fillWidth: true
            implicitHeight: overviewRow.implicitHeight + Theme.spaceLg * 2
            decorated: true

            RowLayout {
                id: overviewRow
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                spacing: Theme.spaceLg

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXs
                    Text {
                        text: root.activeCount > 0 ? "Xenon is working in the background"
                            : root.failureCount > 0 ? "Some activity needs attention"
                            : root.readyCount > 0 ? "Updates are ready"
                            : "You're all caught up"
                        color: Theme.text
                        font.pixelSize: Theme.typeBodyLarge
                        font.weight: Font.DemiBold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: root.activeCount > 0
                            ? "You can leave this page; jobs continue through their owning Xenon services."
                            : root.failureCount > 0
                                ? "Retry failed work below or recheck update sources."
                                : root.readyCount > 0
                                    ? "Verified or available updates are waiting for your action."
                                    : "No download, update or preparation work currently needs action."
                        color: Theme.textMuted
                        font.pixelSize: Theme.typeCaption
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    spacing: Theme.spaceXs
                    XButton {
                        text: "Check for Updates"
                        onClicked: root.checkAll()
                    }

                    XButton {
                        visible: root.failureCount > 0
                        text: "Retry Issues  " + root.failureCount
                        variant: "ghost"
                        onClicked: root.retryAllFailed()
                    }
                    XButton {
                        visible: root.failureCount > 0
                        text: "Recheck Issues"
                        variant: "ghost"
                        onClicked: root.checkAll()
                    }
                }
            }
        }

        XPanel {
            visible: root.showActive
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceMd
            implicitHeight: activeSection.implicitHeight + Theme.spaceLg * 2

            ColumnLayout {
                id: activeSection
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                spacing: Theme.spaceMd

                XSectionHeader {
                    Layout.fillWidth: true
                    title: "Active"
                    description: "Current downloads, installs, checks and game preparation."
                }

                Text {
                    visible: root.activeJobs.length === 0
                    Layout.fillWidth: true
                    text: "No active background jobs."
                    color: Theme.textMuted
                    font.pixelSize: Theme.typeCaption
                }

                Repeater {
                    model: root.activeJobs
                    delegate: XPanel {
                        id: activeJobCard
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: activeJobColumn.implicitHeight + Theme.spaceMd * 2
                        decorated: false
                        color: Theme.highContrast ? Theme.surfaceAlt : Qt.rgba(Theme.surfaceAlt.r, Theme.surfaceAlt.g, Theme.surfaceAlt.b, Theme.panelOpacity)

                        ColumnLayout {
                            id: activeJobColumn
                            anchors.fill: parent
                            anchors.margins: Theme.spaceMd
                            spacing: Theme.spaceSm

                            RowLayout {
                                Layout.fillWidth: true
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Text { text: String(activeJobCard.modelData.title || "Background job"); color: Theme.text; font.pixelSize: Theme.typeBody; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                    Text { Layout.fillWidth: true; text: root.progressText(activeJobCard.modelData); color: Theme.textMuted; font.pixelSize: Theme.typeCaption; elide: Text.ElideRight }
                                    Text {
                                        visible: String(activeJobCard.modelData.message || "").length > 0
                                        Layout.fillWidth: true
                                        text: String(activeJobCard.modelData.message || "")
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.typeCaption
                                        wrapMode: Text.WordWrap
                                        maximumLineCount: 2
                                        elide: Text.ElideRight
                                    }
                                }
                                StatusPill { label: root.statusLabel(String(activeJobCard.modelData.status || "")); tone: Theme.accent }
                                XButton { visible: Boolean(activeJobCard.modelData.canCancel); text: "Cancel"; variant: "danger"; onClicked: root.runJobAction(activeJobCard.modelData, "cancel") }
                            }

                            Rectangle {
                                visible: Number(activeJobCard.modelData.progress || -1) >= 0
                                Layout.fillWidth: true
                                Layout.preferredHeight: 8
                                radius: 4
                                color: Theme.surfaceAlt
                                Rectangle {
                                    width: parent.width * Math.max(0, Math.min(1, Number(activeJobCard.modelData.progress || 0)))
                                    height: parent.height
                                    radius: parent.radius
                                    color: Theme.accent
                                    Behavior on width { enabled: Theme.motionNormal > 0; NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }
                                }
                            }
                        }
                    }
                }
            }
        }

        XPanel {
            visible: root.showIssues
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceLg
            implicitHeight: issueSection.implicitHeight + Theme.spaceLg * 2

            ColumnLayout {
                id: issueSection
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                spacing: Theme.spaceMd

                XSectionHeader { Layout.fillWidth: true; title: "Needs attention"; description: "Failed jobs that can be retried or checked again." }
                Text { visible: root.failedJobs.length === 0; Layout.fillWidth: true; text: "No failed jobs."; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                Repeater {
                    model: root.failedJobs
                    delegate: XSettingsCard {
                        id: failedJobCard
                        required property var modelData
                        Layout.fillWidth: true
                        decorated: false
                        title: String(modelData.title || "Background job")
                        description: String(modelData.message || "The operation failed.")
                        actionWidth: 220
                        XButton { text: root.primaryActionLabel(failedJobCard.modelData); visible: root.primaryAction(failedJobCard.modelData).length > 0; variant: "primary"; onClicked: root.runJobAction(failedJobCard.modelData, root.primaryAction(failedJobCard.modelData)) }
                    }
                }
            }
        }

        XPanel {
            visible: root.showReady
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceLg
            implicitHeight: readySection.implicitHeight + Theme.spaceLg * 2

            ColumnLayout {
                id: readySection
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                spacing: Theme.spaceMd

                XSectionHeader { Layout.fillWidth: true; title: "Ready"; description: "Updates discovered by Xenon that are ready to download or install." }
                Text { visible: root.readyJobs.length === 0; Layout.fillWidth: true; text: "No updates are waiting for action."; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                Repeater {
                    model: root.readyJobs
                    delegate: XSettingsCard {
                        id: readyJobCard
                        required property var modelData
                        Layout.fillWidth: true
                        decorated: false
                        title: String(modelData.title || "Update")
                        description: {
                            var version = String(modelData.subtitle || "")
                            var message = String(modelData.message || "")
                            return (version.length > 0 ? "Version " + version + (message.length > 0 ? " • " : "") : "") + message
                        }
                        actionWidth: 240
                        XButton { text: root.primaryActionLabel(readyJobCard.modelData); visible: root.primaryAction(readyJobCard.modelData).length > 0; variant: "primary"; onClicked: root.runJobAction(readyJobCard.modelData, root.primaryAction(readyJobCard.modelData)) }
                    }
                }
            }
        }

        XPanel {
            visible: root.showHistory
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceLg
            implicitHeight: historySection.implicitHeight + Theme.spaceLg * 2

            ColumnLayout {
                id: historySection
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                spacing: Theme.spaceMd

                RowLayout {
                    Layout.fillWidth: true
                    XSectionHeader { Layout.fillWidth: true; title: "Recent activity"; description: "Bounded module update history." }
                    XButton { visible: root.historyEntries.length > 0; text: "Clear History"; variant: "ghost"; onClicked: launcherBridge.clearModuleUpdateHistory("") }
                }
                Text { visible: root.historyEntries.length === 0; Layout.fillWidth: true; text: "No completed module update activity has been recorded yet."; color: Theme.textMuted; font.pixelSize: Theme.typeCaption }
                Repeater {
                    model: root.historyEntries.slice(0, 20)
                    delegate: XSettingsCard {
                        required property var modelData
                        compact: true
                        decorated: false
                        Layout.fillWidth: true
                        title: String(modelData.moduleName || modelData.moduleId || "Module")
                        description: {
                            var action = String(modelData.action || "Update")
                            var message = String(modelData.message || "")
                            var version = String(modelData.toVersion || "")
                            var stamp = String(modelData.timestamp || "")
                            var suffix = version.length > 0 ? " • " + version : ""
                            if (stamp.length > 0) suffix += " • " + stamp
                            return action + suffix + (message.length > 0 ? "\n" + message : "")
                        }
                        actionWidth: 140
                        StatusPill {
                            label: String(modelData.outcome || "recorded")
                            tone: String(modelData.outcome || "").toLowerCase() === "success" ? Theme.success
                                : (String(modelData.outcome || "").toLowerCase() === "failed" || String(modelData.outcome || "").toLowerCase() === "failure") ? Theme.danger : Theme.textMuted
                        }
                    }
                }
            }
        }
    }
}
