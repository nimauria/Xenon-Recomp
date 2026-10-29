import { describe, it, expect } from "vitest";
import { PreviewTransport, initialState } from "./backend";
describe("converted profile behaviour", () => {
  it("preserves all nine runtime defaults and applies overrides only when isolated", async () => {
    const backend = new PreviewTransport(initialState(), () => {});
    const created = await backend.request("profiles.create", {
      name: "Profile",
      description: "",
      isolatedSettings: true,
      runtimeOverrides: { "audio/masterVolume": 0.35 },
      startupPage: "Support",
    });
    const profile = created.profiles.at(-1)!;
    expect(profile.effectiveRuntimeSettings?.["audio/masterVolume"]).toBe(0.35);
    const activated = await backend.request("profiles.activate", {
      id: profile.id,
    });
    expect(activated.preferences.startPage).toBe("Support");
    const edited = await backend.request("launcher.action", {
      action: "profiles.update",
      data: { id: profile.id, name: "Profile", isolatedSettings: false },
    });
    expect(edited.profiles.at(-1)?.runtimeOverrides).toEqual({});
  });
  it("keeps legacy limits and generates unique repeated duplicates", async () => {
    const backend = new PreviewTransport(initialState(), () => {});
    const created = await backend.request("profiles.create", {
      name: "A".repeat(48),
      description: "B".repeat(180),
    });
    const id = created.profiles.at(-1)!.id;
    await backend.request("launcher.action", {
      action: "profiles.duplicate",
      data: { id },
    });
    const duplicated = await backend.request("launcher.action", {
      action: "profiles.duplicate",
      data: { id },
    });
    expect(
      new Set(duplicated.profiles.map((profile) => profile.name)).size,
    ).toBe(duplicated.profiles.length);
    expect(
      duplicated.profiles.every((profile) => profile.name.length <= 48),
    ).toBe(true);
  });
  it("rejects invalid or unrelated runtime overrides", async () => {
    const backend = new PreviewTransport(initialState(), () => {});
    await expect(
      backend.request("profiles.create", {
        name: "Invalid",
        isolatedSettings: true,
        runtimeOverrides: { "audio/masterVolume": 2 },
      }),
    ).rejects.toThrow("range");
    await expect(
      backend.request("profiles.create", {
        name: "Invalid",
        isolatedSettings: true,
        runtimeOverrides: { "ui/theme": "carbon" },
      }),
    ).rejects.toThrow("Unsupported");
  });
});
