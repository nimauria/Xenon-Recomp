import { afterEach, expect, it, vi } from "vitest";
import { connect } from "./backend";
afterEach(() => vi.unstubAllGlobals());
it("accepts versioned native invalidations and releases the subscriber", () => {
  const target = new EventTarget();
  vi.stubGlobal("window", {
    cefQuery: () => {},
    addEventListener: target.addEventListener.bind(target),
    removeEventListener: target.removeEventListener.bind(target),
  });
  const revisions: number[] = [];
  const unsubscribe = connect().subscribe!((revision) =>
    revisions.push(revision),
  );
  const send = (detail: unknown) =>
    target.dispatchEvent(new CustomEvent("xenon:revision", { detail }));
  send({ version: 99, revision: 1 });
  send({ version: 1, revision: "2" });
  send({ version: 1, revision: -1 });
  send({ version: 1, revision: 7 });
  unsubscribe();
  send({ version: 1, revision: 8 });
  expect(revisions).toEqual([7]);
});
