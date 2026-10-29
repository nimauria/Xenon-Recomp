import { expect, it } from "vitest";
import { commandResults } from "./search";
import { initialState, PreviewTransport } from "./backend";
it("ranks matching titles and requires every search token", () => {
  const state = initialState();
  state.games = [
    {
      id: "ac6",
      title: "Ace Combat 6",
      module: "Gracemeria",
      description: "Flight game",
      favorite: false,
      lastPlayed: null,
    },
  ];
  expect(commandResults(state, "ace combat")[0].id).toBe("game:ac6");
  expect(commandResults(state, "ace nonexistent")).toEqual([]);
  expect(commandResults(state, "").some((item) => item.id === "game:ac6")).toBe(
    false,
  );
  expect(commandResults(state, "master volume")[0].intent).toEqual({
    type: "navigate",
    page: "Settings",
    targetId: "audio",
  });
  expect(commandResults(state, "", 500).length).toBeLessThanOrEqual(50);
});
it("preserves startup precedence when restoring the remembered page", async () => {
  const state = initialState();
  state.profiles[0].startupPage = "Support";
  const transport = new PreviewTransport(state, () => {});
  await transport.request("launcher.action", {
    action: "navigation.remember",
    data: { page: "Captures" },
  });
  expect(
    (await transport.request("application.snapshot", {})).preferences.startPage,
  ).toBe("Support");
  await transport.request("launcher.action", {
    action: "settings.set",
    data: { key: "general/restoreLastPage", value: true },
  });
  expect(
    (await transport.request("application.snapshot", {})).preferences.startPage,
  ).toBe("Captures");
});
