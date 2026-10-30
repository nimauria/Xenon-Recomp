import { it, expect } from "vitest";
import { initialState } from "./backend";
import { gamePolicy, profilePolicy } from "./policies";
it("keeps menu and inline availability aligned with session and profile ownership", () => {
  const state = initialState(),
    game = {
      id: "one",
      title: "Game",
      module: "",
      description: "",
      favorite: false,
      lastPlayed: null,
    };
  expect(gamePolicy(state, game, false).launch).toBe(false);
  state.mode = "native";
  state.capabilities.runtime = true;
  expect(gamePolicy(state, game, false).launch).toBe(true);
  state.session = {
    sessionId: "test",
    gameId: "one",
    state: "running",
    fixture: true,
  };
  expect(gamePolicy(state, game, false)).toMatchObject({
    launch: false,
    edit: false,
    remove: false,
    favorite: true,
  });
  expect(profilePolicy(state, state.profiles[0], false).remove).toBe(false);
  expect(
    profilePolicy(state, { ...state.profiles[0], id: "second" }, false).remove,
  ).toBe(true);
  expect(gamePolicy(state, game, true).favorite).toBe(false);
});
