pragma Singleton

import QtQuick

// UI-side selection/cache only. Profile creation, validation, persistence,
// import/export, avatar storage and test fixtures all live in FrontendBackend.
QtObject {
    id: root

    property int activeIndex: 0
    property int selectedIndex: 0
    property var profiles: []
    readonly property int profileNameLimit: launcherBridge.profileNameLimit
    readonly property int descriptionLimit: launcherBridge.profileDescriptionLimit

    function reloadBackend(preferredIndex) {
        var items = launcherBridge.profileEntries()
        profiles = items
        activeIndex = Math.max(0, Math.min(launcherBridge.activeProfileIndex(), Math.max(0, items.length - 1)))
        if (preferredIndex === undefined || preferredIndex === null)
            selectedIndex = Math.max(0, Math.min(selectedIndex, Math.max(0, items.length - 1)))
        else
            selectedIndex = Math.max(0, Math.min(Number(preferredIndex), Math.max(0, items.length - 1)))
    }

    function reset(primaryName, testMode) {
        reloadBackend(launcherBridge.activeProfileIndex())
    }

    function profile(index) {
        if (profiles.length === 0)
            return { profileId: "", profileName: "", description: "", active: false,
                     games: 0, modules: 0, saveSets: 0, avatarPath: "",
                     gamePath: "", savePath: "", screenshotPath: "",
                     effectiveGamePath: "", effectiveSavePath: "", effectiveScreenshotPath: "",
                     storagePath: "", storageOpenable: false, pathOverrideCount: 0,
                     region: "Auto (Global)", startupPage: "Launcher default", offline: true,
                     isolatedSettings: false, runtimeOverrides: ({}), runtimeOverrideCount: 0,
                     effectiveRuntimeSettings: ({}), createdAt: "", lastUsedAt: "", lastUsed: "" }
        var safe = Math.max(0, Math.min(index, profiles.length - 1))
        return profiles[safe]
    }

    function names() {
        var result = []
        for (var i = 0; i < profiles.length; ++i)
            result.push(profiles[i].profileName)
        return result
    }

    function nameAvailable(name, excludeIndex) {
        return launcherBridge.profileNameAvailable(String(name || ""), excludeIndex)
    }

    function createFromData(data) {
        var backendIndex = launcherBridge.createProfile(data)
        if (backendIndex >= 0)
            reloadBackend(backendIndex)
        return backendIndex
    }

    function updateFromData(index, data) {
        var ok = launcherBridge.updateProfile(index, data)
        if (ok)
            reloadBackend(index)
        return ok
    }

    function duplicate(index) {
        var backendIndex = launcherBridge.duplicateProfile(index)
        if (backendIndex >= 0)
            reloadBackend(backendIndex)
        return backendIndex
    }

    function activate(index) {
        var ok = launcherBridge.activateProfile(index)
        if (ok)
            reloadBackend(index)
        return ok
    }

    function remove(index) {
        var preferred = Math.max(0, Math.min(index, profiles.length - 2))
        var ok = launcherBridge.removeProfile(index)
        if (ok)
            reloadBackend(preferred)
        return ok
    }

    property Connections backendConnections: Connections {
        target: launcherBridge
        function onProfilesChanged() {
            root.reloadBackend(root.selectedIndex)
        }
    }
}
