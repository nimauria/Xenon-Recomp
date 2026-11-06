import { describe, expect, it } from "vitest";
import {
  gameSettingsDraft,
  inheritedGameSettings,
  validateGameSettings,
} from "./gameSettings";
import type { Snapshot, Module } from "../contracts/launcher";
describe("per-game settings", () => {
  it("inherits launcher/profile values and keeps sparse game drafts isolated", () => {
    const state: Snapshot = {
      revision: 0,
      mode: "preview",
      games: [],
      preferences: {
        theme: "dark",
        reducedMotion: false,
        startPage: "Library",
      },
      capabilities: {
        runtime: false,
        importContent: false,
        modules: false,
        captures: false,
      },
      settings: { "audio/masterVolume": 0.8 },
      activeProfileId: "one",
      profiles: [
        {
          id: "one",
          name: "One",
          description: "",
          isolatedSettings: true,
          runtimeOverrides: { "audio/masterVolume": 0.5 },
        },
      ],
    };
    expect(inheritedGameSettings(state)["audio/masterVolume"]).toBe(0.5);
    const game = {
      id: "game",
      title: "Game",
      module: "Module",
      description: "",
      favorite: false,
      lastPlayed: null,
      moduleId: "one",
      runtimeOverrides: { "audio/masterVolume": 0.2 },
      moduleOverrides: {
        one: { difficulty: "hard" },
        two: { difficulty: "easy" },
      },
    };
    const draft = gameSettingsDraft(game);
    draft.runtime["audio/masterVolume"] = 0.4;
    expect(game.runtimeOverrides["audio/masterVolume"]).toBe(0.2);
    expect(draft.module).toEqual({ difficulty: "hard" });
  });
  it("rejects invalid runtime and module values at the preview boundary", () => {
    expect(() =>
      validateGameSettings({ "audio/masterVolume": 2 }, {}),
    ).toThrow();
    expect(() =>
      validateGameSettings({ "input/rumble": "false" }, {}),
    ).toThrow();
    expect(() => validateGameSettings({ "ui/theme": "dark" }, {})).toThrow();
    expect(() => validateGameSettings([], {})).toThrow();
    const module = {
      schema: [
        { id: "difficulty", type: "choice", options: ["normal", "hard"] },
        { id: "fov", type: "number", minimum: 60, maximum: 120 },
      ],
    } as Module;
    expect(() =>
      validateGameSettings({}, { difficulty: "fake" }, module),
    ).toThrow();
    expect(() => validateGameSettings({}, { fov: 130 }, module)).toThrow();
    expect(() => validateGameSettings({}, { removed: true }, module)).toThrow();
    expect(
      validateGameSettings(
        { "audio/masterVolume": 0.2 },
        { difficulty: "hard" },
        module,
      ).runtime,
    ).toEqual({ "audio/masterVolume": 0.2 });
  });
});
