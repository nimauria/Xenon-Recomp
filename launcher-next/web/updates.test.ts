import { describe, it, expect } from "vitest";
import { installBlock, updatePackages } from "./updates";
import type { Snapshot } from "../contracts/launcher";
describe("install availability", () => {
  it("keeps unsupported, unchecked, unsigned, active, manual and already-installed states distinct", () => {
    expect(installBlock(undefined, false, false)).toBe("unsupported");
    expect(installBlock(undefined, false)).toBe("check");
    expect(installBlock({ state: "digest-unavailable" }, false)).toBe(
      "unavailable",
    );
    expect(installBlock({ state: "downloading", busy: true }, false)).toBe(
      "busy",
    );
    expect(installBlock({ state: "up-to-date" }, false)).toBe("installed");
    expect(
      installBlock({ state: "ready-to-install", canInstall: false }, true),
    ).toBe("manual");
    expect(
      installBlock({ state: "ready-to-install", canInstall: true }, true),
    ).toBe("");
  });
  it("keeps verified packages discoverable after completed download records are cleared", () => {
    const state = {
      modules: [],
      jobs: [],
      catalog: { entries: [{ id: "test", name: "Example module" }] },
      moduleUpdates: { test: { state: "ready-to-install", canInstall: true } },
    } as unknown as Snapshot;
    expect(updatePackages(state).find((p) => p.id === "test")).toMatchObject({
      name: "Example module",
      value: { canInstall: true },
    });
  });
});
