import { describe, expect, it } from "vitest";
import { initialState, PreviewTransport, validateState } from "./backend";

describe("launcher preview contract", () => {
  it("does not fabricate installed games or runtime capabilities", async () => {
    const backend = new PreviewTransport(initialState(), () => {});
    const state = await backend.request("application.snapshot", {});
    expect(state.games).toEqual([]);
    expect(state.capabilities.runtime).toBe(false);
    await expect(
      backend.request("session.launch", { gameId: "missing" }),
    ).rejects.toThrow("not connected");
  });
  it("preserves active identity when removal or activation fails", async () => {
    const backend = new PreviewTransport(initialState(), () => {});
    await expect(
      backend.request("profiles.remove", { id: "local" }),
    ).rejects.toThrow("Activate another");
    await expect(
      backend.request("profiles.activate", { id: "missing" }),
    ).rejects.toThrow("no longer exists");
    expect(
      (await backend.request("application.snapshot", {})).activeProfileId,
    ).toBe("local");
  });
  it("creates stable IDs and rejects duplicate names", async () => {
    const backend = new PreviewTransport(initialState(), () => {});
    const created = await backend.request("profiles.create", {
      name: " Pilot ",
    });
    expect(created.profiles[1].name).toBe("Pilot");
    expect(created.profiles[1].id).not.toBe("Pilot");
    await expect(
      backend.request("profiles.create", { name: "pilot" }),
    ).rejects.toThrow("already in use");
  });
  it("does not commit a mutation when persistence fails", async () => {
    const backend = new PreviewTransport(initialState(), () => {
      throw new Error("Storage full");
    });
    await expect(
      backend.request("settings.update", { theme: "light" }),
    ).rejects.toThrow("Storage full");
    expect(
      (await backend.request("application.snapshot", {})).preferences.theme,
    ).toBe("dark");
  });
  it("rejects malformed persistence instead of overwriting it", () => {
    expect(() =>
      validateState({ ...initialState(), activeProfileId: "missing" }),
    ).toThrow();
    expect(() =>
      validateState({ ...initialState(), preferences: { theme: "unknown" } }),
    ).toThrow();
  });
});
