import { describe, it, expect } from "vitest";
import { profileNameKey, trimProfileName } from "./profileNames";
import { PreviewTransport, initialState } from "./backend";
describe("pinned profile identity", () => {
  it("keeps older equivalent display names editable without forced renaming", async () => {
    const state = initialState();
    state.profiles.push(
      { id: "old-a", name: "\u00c9lite", description: "" },
      { id: "old-b", name: "\u00e9LITE", description: "" },
    );
    const backend = new PreviewTransport(state, () => {});
    const updated = await backend.request("launcher.action", {
      action: "profiles.update",
      data: { id: "old-b", name: "\u00e9LITE", description: "Kept" },
    });
    expect(updated.profiles.find((p) => p.id === "old-b")?.description).toBe(
      "Kept",
    );
    expect(updated.profiles.find((p) => p.id === "old-b")?.name).toBe(
      "\u00e9LITE",
    );
    await expect(
      backend.request("profiles.create", {
        name: "\u00c9LITE",
        description: "",
      }),
    ).rejects.toThrow("in use");
  });
  it("uses locale-independent scalar lowercase without folding or normalization", () => {
    expect(profileNameKey("\u00c9LITE")).toBe(profileNameKey("\u00e9lite"));
    expect(profileNameKey("\u041f\u0418\u041b\u041e\u0422")).toBe(
      "\u043f\u0438\u043b\u043e\u0442",
    );
    expect(profileNameKey("\u039f\u03a3")).toBe("\u03bf\u03c3");
    expect(profileNameKey("\u0130")).toBe("i\u0307");
    expect(profileNameKey("\u{10400}")).toBe("\u{10428}");
    expect(profileNameKey("Stra\u00dfe")).not.toBe(profileNameKey("STRASSE"));
    expect(profileNameKey("\u00c9")).not.toBe(profileNameKey("E\u0301"));
    expect(trimProfileName("\u0085\u00a0 Pilot \u3000")).toBe("Pilot");
    expect(() => profileNameKey("\ud800")).toThrow("Unicode");
    expect(() => profileNameKey("a\0b")).toThrow("NUL");
  });
  it("applies the same identity to preview create/update/duplicate", async () => {
    const backend = new PreviewTransport(initialState(), () => {});
    const created = await backend.request("profiles.create", {
      name: "\u00c9lite",
      description: "",
    });
    await expect(
      backend.request("profiles.create", {
        name: "\u00e9LITE",
        description: "",
      }),
    ).rejects.toThrow("in use");
    const copy = await backend.request("launcher.action", {
      action: "profiles.duplicate",
      data: { id: created.profiles.at(-1)!.id },
    });
    expect(copy.profiles.at(-1)!.name).toBe("\u00c9lite copy");
    await expect(
      backend.request("launcher.action", {
        action: "profiles.update",
        data: { id: copy.profiles.at(-1)!.id, name: "\u00e9LITE" },
      }),
    ).rejects.toThrow("unique");
  });
});
