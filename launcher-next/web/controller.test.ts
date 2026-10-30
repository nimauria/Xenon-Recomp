import { describe, it, expect } from "vitest";
import {
  ControllerRepeater,
  controllerFamily,
  controllerGlyphs,
  padAction,
  spatialTarget,
} from "./controller";
describe("controller navigation", () => {
  it("maps standard pads and requires a mapping for unknown layouts", () => {
    const buttons = Array.from({ length: 16 }, () => ({
      pressed: false,
      touched: false,
      value: 0,
    }));
    const pad = {
      buttons,
      axes: [0, 0],
      mapping: "standard" as GamepadMappingType,
    };
    buttons[0].pressed = true;
    expect(padAction(pad)).toBe("activate");
    expect(padAction({ ...pad, mapping: "" })).toBeUndefined();
    buttons[0].pressed = false;
    pad.axes = [0.7, -0.1];
    expect(padAction(pad)).toBe("right");
    pad.axes = [0.1, -0.7];
    expect(padAction(pad)).toBe("up");
    pad.axes = [0.2, 0.2];
    expect(padAction(pad)).toBeUndefined();
  });
  it("activates once per press while directions repeat after a delay", () => {
    const r = new ControllerRepeater();
    expect(r.step("activate", 0)).toBe("activate");
    expect(r.step("activate", 1000)).toBeUndefined();
    r.step(undefined, 1001);
    expect(r.step("activate", 1002)).toBe("activate");
    expect(r.step("right", 1100)).toBe("right");
    expect(r.step("right", 1449)).toBeUndefined();
    expect(r.step("right", 1450)).toBe("right");
    expect(r.step("right", 1500)).toBeUndefined();
    expect(r.step("right", 1550)).toBe("right");
  });
  it("moves spatially through rows and does not reverse direction at an edge", () => {
    const at = (left: number, top: number) => ({
      left,
      top,
      width: 100,
      height: 80,
    });
    const boxes = [
      at(0, 0),
      at(110, 0),
      at(220, 0),
      at(0, 90),
      at(110, 90),
      at(220, 90),
    ];
    expect(spatialTarget(boxes[1], boxes, "down")).toBe(4);
    expect(spatialTarget(boxes[1], boxes, "right")).toBe(2);
    expect(spatialTarget(boxes[1], boxes, "up")).toBe(-1);
    expect(spatialTarget(boxes[4], boxes, "left")).toBe(3);
  });
  it("labels the physical standard-layout buttons for the connected device", () => {
    expect(
      controllerGlyphs(controllerFamily("Sony DualSense 054c")).activate,
    ).toBe("Cross");
    expect(
      controllerGlyphs(controllerFamily("Nintendo Switch Pro")).activate,
    ).toBe("B");
    expect(controllerGlyphs(controllerFamily("Xbox XInput")).activate).toBe(
      "A",
    );
    expect(
      controllerGlyphs(controllerFamily("Unknown controller")).activate,
    ).toBe("1");
  });
});
