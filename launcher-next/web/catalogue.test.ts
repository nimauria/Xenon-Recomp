import { expect, it } from "vitest";
import { initialState } from "./backend";
import { catalogueEntries } from "./catalogue";
import { attentionItems } from "./attention";

it("combines catalogue words and native package/platform states without asserting compatibility", () => {
  const state = initialState();
  state.modules = [{ id: "one" }] as typeof state.modules;
  state.catalog = {
    state: "ready",
    entries: [
      {
        id: "one",
        name: "Orbital Flight",
        description: "Space exploration",
        publisher: "Xenon",
        assetName: "one.zip",
      },
      { id: "two", name: "Rally", description: "Racing", publisher: "Xenon" },
    ],
  } as typeof state.catalog;
  state.moduleUpdates = {
    two: { state: "ready-to-install", canInstall: false },
  };
  expect(
    catalogueEntries(state, "space orbital", "platform").map((e) => e.id),
  ).toEqual(["one"]);
  expect(catalogueEntries(state, "", "installed").map((e) => e.id)).toEqual([
    "one",
  ]);
  expect(catalogueEntries(state, "", "ready").map((e) => e.id)).toEqual([
    "two",
  ]);
  expect(catalogueEntries(state, "not present", "all")).toEqual([]);
});

it("Home attention counts actionable state, excludes uninstalled release offers, and routes without mutations", () => {
  const state = initialState();
  expect(attentionItems(state)).toEqual([]);
  state.modules = [{ id: "installed" }] as typeof state.modules;
  state.moduleUpdates = {
    installed: { state: "update-available" },
    unrelated: { state: "available" },
    package: { state: "ready-to-install", canInstall: false },
  };
  state.launcherUpdate = { state: "available" };
  state.jobs = [
    { state: "failed" },
    { state: "cancelled" },
    { state: "completed" },
  ] as typeof state.jobs;
  state.games = [
    { media: [{ missing: true }, { missing: true }] },
  ] as typeof state.games;
  const before = JSON.stringify(state);
  expect(attentionItems(state)).toEqual([
    { key: "ready", count: 1, page: "Downloads" },
    { key: "updates", count: 2, page: "Updates" },
    { key: "failed", count: 1, page: "Downloads" },
    { key: "sources", count: 1, page: "Library" },
  ]);
  expect(JSON.stringify(state)).toBe(before);
});
