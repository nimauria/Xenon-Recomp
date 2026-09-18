pragma Singleton

import QtQuick

QtObject {
    id: root

    property int activeIndex: 0
    property int selectedIndex: 0
    property int nextProfileNumber: 2
    property var profiles: []

    function newProfileId() {
        return "profile-" + Date.now().toString(36) + "-" + nextProfileNumber.toString(36)
    }

    function nowIso() {
        return new Date().toISOString()
    }

    function nameAvailable(name, excludeIndex) {
        var needle = String(name || "").trim().toLowerCase()
        if (needle.length === 0)
            return false
        for (var i = 0; i < profiles.length; ++i) {
            if (i === excludeIndex)
                continue
            if (String(profiles[i].profileName || "").trim().toLowerCase() === needle)
                return false
        }
        return true
    }

    function uniqueCopyName(baseName) {
        var rootName = String(baseName || "Profile") + " Copy"
        if (nameAvailable(rootName, -1))
            return rootName
        var suffix = 2
        while (!nameAvailable(rootName + " " + suffix, -1))
            suffix += 1
        return rootName + " " + suffix
    }

    function persist() {
        // Test fixtures deliberately never leak into a normal launcher profile store.
        if (!launcherBridge.testMode)
            launcherBridge.saveProfileState(JSON.stringify(profiles))
    }

    function restore(primaryName) {
        var raw = String(launcherBridge.loadProfileState())
        if (raw.length === 0)
            return false

        try {
            var parsed = JSON.parse(raw)
            if (!Array.isArray(parsed) || parsed.length === 0)
                return false

            var copy = []
            var foundActive = false
            var active = 0
            for (var i = 0; i < parsed.length; ++i) {
                var source = parsed[i] || {}
                var item = makeProfile(
                    String(source.profileName || (i === 0 ? primaryName : "Profile " + (i + 1))),
                    String(source.description || "Xenon launcher profile."),
                    Boolean(source.active) && !foundActive,
                    String(source.profileId || ("profile-restored-" + i)))

                item.games = Number(source.games || 0)
                item.modules = Number(source.modules || 0)
                item.saveSets = Number(source.saveSets || 0)
                item.avatarPath = String(source.avatarPath || "")
                item.gamePath = String(source.gamePath || "")
                item.savePath = String(source.savePath || "")
                item.screenshotPath = String(source.screenshotPath || "")
                item.region = String(source.region || "Auto (Global)")
                item.startupPage = String(source.startupPage || "Library")
                item.offline = source.offline === undefined ? true : Boolean(source.offline)
                item.isolatedSettings = Boolean(source.isolatedSettings)
                item.createdAt = String(source.createdAt || source.created || nowIso())
                item.lastUsedAt = String(source.lastUsedAt || "")
                item.lastUsed = String(source.lastUsed || (item.active ? "Current session" : "Not used yet"))

                if (item.active) {
                    foundActive = true
                    active = i
                }
                copy.push(item)
            }

            if (!foundActive) {
                copy[0].active = true
                active = 0
            }

            profiles = copy
            activeIndex = active
            selectedIndex = active
            nextProfileNumber = profiles.length + 1
            launcherBridge.profileName = profiles[active].profileName
            return true
        } catch (error) {
            launcherBridge.notify("Profile data reset", "Saved front-end profile data could not be read, so Xenon created a clean profile list.")
            return false
        }
    }

    function makeProfile(name, description, active, profileId) {
        return {
            profileId: profileId || newProfileId(),
            profileName: name,
            description: description,
            active: active,
            games: 0,
            avatarPath: "",
            modules: 0,
            saveSets: 0,
            gamePath: "",
            savePath: "",
            screenshotPath: "",
            region: "Auto (Global)",
            startupPage: "Library",
            offline: true,
            isolatedSettings: false,
            createdAt: nowIso(),
            lastUsedAt: active ? nowIso() : "",
            lastUsed: active ? "Current session" : "Not used yet"
        }
    }

    function reset(primaryName, testMode) {
        if (!testMode && restore(primaryName))
            return

        var items = [makeProfile(primaryName, "Primary Xenon launcher profile.", true, "profile-primary")]
        if (testMode) {
            items.push({
                profileId: "profile-test",
                profileName: "Test Profile",
                description: "Fictional profile used to exercise profile switching and editing.",
                active: false,
                games: 2,
                avatarPath: "",
                modules: 3,
                saveSets: 1,
                gamePath: "TEST://Games",
                savePath: "TEST://Saves",
                screenshotPath: "TEST://Screenshots",
                region: "Auto (Global)",
                startupPage: "Library",
                offline: true,
                isolatedSettings: true,
                createdAt: nowIso(),
                lastUsedAt: nowIso(),
                lastUsed: "Test fixture"
            })
        }
        profiles = items
        activeIndex = 0
        selectedIndex = 0
        nextProfileNumber = 2
    }

    function profile(index) {
        if (profiles.length === 0)
            return makeProfile("", "", false, "")
        var safe = Math.max(0, Math.min(index, profiles.length - 1))
        return profiles[safe]
    }

    function names() {
        var result = []
        for (var i = 0; i < profiles.length; ++i)
            result.push(profiles[i].profileName)
        return result
    }

    function replace(index, item) {
        if (index < 0 || index >= profiles.length)
            return
        var copy = profiles.slice(0)
        copy[index] = item
        profiles = copy
        persist()
    }

    function patch(index, key, value) {
        if (index < 0 || index >= profiles.length)
            return
        var item = Object.assign({}, profiles[index])
        item[key] = value
        replace(index, item)
    }

    function createFromData(data) {
        var requestedName = String(data.profileName || ("Profile " + nextProfileNumber)).trim()
        if (!nameAvailable(requestedName, -1))
            return -1
        var item = makeProfile(
            requestedName,
            String(data.description || "Xenon launcher profile."),
            false,
            String(data.profileId || newProfileId()))
        item.avatarPath = String(data.avatarPath || "")
        item.gamePath = String(data.gamePath || "")
        item.savePath = String(data.savePath || "")
        item.screenshotPath = String(data.screenshotPath || "")
        item.region = String(data.region || "Auto (Global)")
        item.startupPage = String(data.startupPage || "Library")
        item.offline = data.offline === undefined ? true : Boolean(data.offline)
        item.isolatedSettings = Boolean(data.isolatedSettings)

        nextProfileNumber += 1
        var copy = profiles.slice(0)
        copy.push(item)
        profiles = copy
        selectedIndex = profiles.length - 1
        persist()
        return selectedIndex
    }

    function updateFromData(index, data) {
        if (index < 0 || index >= profiles.length)
            return false
        var requestedName = String(data.profileName || "").trim()
        if (!nameAvailable(requestedName, index))
            return false
        var current = profiles[index]
        var item = Object.assign({}, current)
        item.profileName = requestedName.length > 0 ? requestedName : current.profileName
        item.description = String(data.description || "")
        item.avatarPath = data.avatarPath === undefined ? String(current.avatarPath || "") : String(data.avatarPath)
        item.gamePath = String(data.gamePath || "")
        item.savePath = String(data.savePath || "")
        item.screenshotPath = String(data.screenshotPath || "")
        item.region = String(data.region || "Auto (Global)")
        item.startupPage = String(data.startupPage || "Library")
        item.offline = Boolean(data.offline)
        item.isolatedSettings = Boolean(data.isolatedSettings)
        replace(index, item)
        if (item.active)
            launcherBridge.profileName = item.profileName
        return true
    }

    function duplicate(index) {
        if (index < 0 || index >= profiles.length)
            return
        var p = profiles[index]
        var item = Object.assign({}, p)
        item.profileId = newProfileId()
        item.profileName = uniqueCopyName(p.profileName)
        item.avatarPath = p.avatarPath && String(p.avatarPath).length > 0
            ? launcherBridge.importProfileAvatar(item.profileId, p.avatarPath)
            : ""
        item.active = false
        item.createdAt = nowIso()
        item.lastUsedAt = ""
        item.lastUsed = "Not used yet"
        nextProfileNumber += 1
        var copy = profiles.slice(0)
        copy.push(item)
        profiles = copy
        selectedIndex = profiles.length - 1
        persist()
    }

    function activate(index) {
        if (index < 0 || index >= profiles.length)
            return
        var copy = []
        for (var i = 0; i < profiles.length; ++i) {
            var item = Object.assign({}, profiles[i])
            item.active = i === index
            if (item.active) {
                item.lastUsedAt = nowIso()
                item.lastUsed = "Current session"
            }
            copy.push(item)
        }
        profiles = copy
        activeIndex = index
        selectedIndex = index
        launcherBridge.profileName = profiles[index].profileName
        persist()
    }

    function remove(index) {
        if (index < 0 || index >= profiles.length || profiles.length <= 1)
            return false
        if (profiles[index].active)
            return false

        var copy = profiles.slice(0)
        copy.splice(index, 1)
        profiles = copy
        selectedIndex = Math.max(0, Math.min(selectedIndex, profiles.length - 1))
        if (activeIndex > index)
            activeIndex -= 1
        persist()
        return true
    }
}
