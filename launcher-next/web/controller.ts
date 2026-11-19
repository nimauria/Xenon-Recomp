import { useEffect, useRef, useState } from "react";
export type Direction = "up" | "down" | "left" | "right";
export type ControllerAction =
  | Direction
  | "activate"
  | "back"
  | "context"
  | "scrollUp"
  | "scrollDown"
  | "pageBack"
  | "pageForward"
  | "quickCenter"
  | "search"
  | "secondary";
export type ControllerFamily = "xbox" | "playstation" | "nintendo" | "generic";
export function controllerFamily(id: string): ControllerFamily {
  const name = id.toLowerCase();
  return /sony|054c|dualshock|dualsense|playstation/.test(name)
    ? "playstation"
    : /nintendo|057e|switch pro|joy-con/.test(name)
      ? "nintendo"
      : /xbox|xinput|045e/.test(name)
        ? "xbox"
        : "generic";
}
export function controllerGlyphs(family: ControllerFamily) {
  return family === "playstation"
    ? { activate: "Cross", back: "Circle", context: "Triangle" }
    : family === "nintendo"
      ? { activate: "B", back: "A", context: "X" }
      : family === "xbox"
        ? { activate: "A", back: "B", context: "Y" }
        : { activate: "1", back: "2", context: "4" };
}
type Pad = Pick<Gamepad, "buttons" | "axes" | "mapping">;
export function padAction(pad: Pad): ControllerAction | undefined {
  if (pad.mapping !== "standard") return; // Unknown layouts need a provider mapping.
  const b = (index: number) => pad.buttons[index]?.pressed;
  if (b(1)) return "back";
  if (b(0)) return "activate";
  if (b(3)) return "context";
  if (b(2)) return "secondary";
  if (b(4)) return "pageBack";
  if (b(5)) return "pageForward";
  if (b(6)) return "scrollUp";
  if (b(7)) return "scrollDown";
  if (b(8)) return "search";
  if (b(9)) return "quickCenter";
  const x = pad.axes[0] ?? 0,
    y = pad.axes[1] ?? 0;
  if (b(12)) return "up";
  if (b(13)) return "down";
  if (b(14)) return "left";
  if (b(15)) return "right";
  if (Math.max(Math.abs(x), Math.abs(y)) < 0.6) return;
  return Math.abs(x) > Math.abs(y)
    ? x < 0
      ? "left"
      : "right"
    : y < 0
      ? "up"
      : "down";
}
export class ControllerRepeater {
  private previous?: ControllerAction;
  private next = 0;
  step(action: ControllerAction | undefined, now: number) {
    if (!action) {
      this.previous = undefined;
      this.next = 0;
      return;
    }
    if (action !== this.previous) {
      this.previous = action;
      this.next = now + 350;
      return action;
    }
    if (
      !["up", "down", "left", "right", "scrollUp", "scrollDown"].includes(
        action,
      ) ||
      now < this.next
    )
      return;
    this.next = now + 100;
    return action;
  }
}
export interface FocusBox {
  left: number;
  top: number;
  width: number;
  height: number;
}
export function spatialTarget(
  current: FocusBox,
  candidates: FocusBox[],
  direction: Direction,
) {
  const horizontal = direction === "left" || direction === "right",
    sign = direction === "left" || direction === "up" ? -1 : 1;
  const primary = (r: FocusBox) =>
    horizontal ? r.left + r.width / 2 : r.top + r.height / 2;
  const secondary = (r: FocusBox) =>
    horizontal ? r.top + r.height / 2 : r.left + r.width / 2;
  let best = -1,
    score = Infinity;
  candidates.forEach((rect, index) => {
    const distance = (primary(rect) - primary(current)) * sign;
    if (distance <= 1) return;
    const cross = Math.abs(secondary(rect) - secondary(current));
    const overlap =
      cross <
      ((horizontal ? rect.height : rect.width) +
        (horizontal ? current.height : current.width)) /
        2;
    const candidate = distance + cross * (overlap ? 1 : 4);
    if (candidate < score) {
      score = candidate;
      best = index;
    }
  });
  return best;
}
const focusable = "button,a[href],input,select,textarea,[tabindex]";
function visible(element: HTMLElement) {
  return (
    element.tabIndex >= 0 &&
    !element.matches(":disabled") &&
    !element.closest("[inert],[aria-hidden=true]") &&
    element.getClientRects().length > 0 &&
    getComputedStyle(element).visibility !== "hidden"
  );
}
export function controllerAction(action: ControllerAction, back: () => void) {
  if (
    document.querySelector("dialog[open],[role=menu]:popover-open") &&
    ["pageBack", "pageForward", "quickCenter", "search", "secondary"].includes(
      action,
    )
  )
    return;
  if (action === "pageBack" || action === "pageForward") {
    window.dispatchEvent(
      new CustomEvent("launcher-cycle-page", {
        detail: action === "pageBack" ? -1 : 1,
      }),
    );
    return;
  }
  if (action === "search") {
    document
      .querySelector<HTMLInputElement>("header input[type=search]")
      ?.focus();
    return;
  }
  if (action === "quickCenter") {
    document
      .querySelector<HTMLButtonElement>("button[data-quick-actions]")
      ?.click();
    return;
  }
  if (action === "secondary") {
    document
      .querySelector<HTMLButtonElement>(
        "[data-controller-secondary]:not(:disabled)",
      )
      ?.click();
    return;
  }
  const active =
    document.activeElement instanceof HTMLElement
      ? document.activeElement
      : undefined;
  const scope =
    document.querySelector<HTMLElement>("[role=menu]:popover-open") ??
    document.querySelector<HTMLDialogElement>("dialog[open]") ??
    document;
  if (action === "back") {
    if (scope instanceof HTMLElement && scope.matches("[role=menu]")) {
      scope.dispatchEvent(
        new KeyboardEvent("keydown", {
          key: "Escape",
          bubbles: true,
          cancelable: true,
        }),
      );
      return;
    }
    if (scope instanceof HTMLDialogElement)
      scope.dispatchEvent(new Event("cancel", { cancelable: true }));
    else back();
    return;
  }
  if (action === "context") {
    if (scope instanceof HTMLElement && scope.matches("[role=menu]")) return;
    active?.dispatchEvent(
      new MouseEvent("contextmenu", {
        bubbles: true,
        cancelable: true,
        clientX: active.getBoundingClientRect().left,
        clientY: active.getBoundingClientRect().bottom,
      }),
    );
    return;
  }
  if (action === "activate") {
    if (active && scope.contains(active) && visible(active)) active.click();
    return;
  }
  if (action === "scrollUp" || action === "scrollDown") {
    const scroller =
      active?.closest<HTMLElement>(".virtual-games,.content-scroll,dialog") ??
      document.querySelector<HTMLElement>("#content");
    scroller?.scrollBy({
      top: (action === "scrollUp" ? -1 : 1) * scroller.clientHeight * 0.8,
      behavior: "instant",
    });
    return;
  }
  if (active && scope.contains(active)) {
    const event = new KeyboardEvent("keydown", {
      key: "Arrow" + action[0].toUpperCase() + action.slice(1),
      bubbles: true,
      cancelable: true,
    });
    if (!active.dispatchEvent(event)) return;
    if (
      (action === "left" || action === "right") &&
      active instanceof HTMLSelectElement &&
      !active.disabled
    ) {
      const options = Array.from(active.options).filter(
        (option) => !option.disabled,
      );
      const index = options.findIndex(
          (option) => option.value === active.value,
        ),
        option = options[index + (action === "left" ? -1 : 1)];
      if (option) {
        active.value = option.value;
        active.dispatchEvent(new Event("change", { bubbles: true }));
      }
      return;
    }
    if (
      (action === "left" || action === "right") &&
      active instanceof HTMLInputElement &&
      active.type === "range" &&
      !active.disabled
    ) {
      const value = active.value;
      if (action === "left") active.stepDown();
      else active.stepUp();
      const next = active.value;
      Object.getOwnPropertyDescriptor(
        HTMLInputElement.prototype,
        "value",
      )!.set!.call(active, value);
      Object.getOwnPropertyDescriptor(
        HTMLInputElement.prototype,
        "value",
      )!.set!.call(active, next);
      // Range controls also commit on blur; native change/input notify controlled controls.
      active.dispatchEvent(new Event("input", { bubbles: true }));
      active.dispatchEvent(new Event("change", { bubbles: true }));
      return;
    }
  }
  const elements = Array.from(
    scope.querySelectorAll<HTMLElement>(focusable),
  ).filter(visible);
  if (!elements.length) return;
  const index =
    active && scope.contains(active)
      ? spatialTarget(
          active.getBoundingClientRect(),
          elements.map((e) => e.getBoundingClientRect()),
          action,
        )
      : -1;
  const next =
    index >= 0
      ? elements[index]
      : active && elements.includes(active)
        ? undefined
        : elements[0];
  next?.focus();
  next?.scrollIntoView({
    block: "nearest",
    inline: "nearest",
    behavior: "instant",
  });
}
export function useControllerNavigation(back: () => void) {
  const callback = useRef(back);
  callback.current = back;
  const [controller, setController] = useState<{
    connected: boolean;
    family: ControllerFamily;
    standard: boolean;
  }>({ connected: false, family: "generic", standard: false });
  useEffect(() => {
    let frame = 0,
      signature = "";
    const repeater = new ControllerRepeater();
    function poll(now: number) {
      const pads = Array.from(navigator.getGamepads?.() ?? []).filter(
        (pad): pad is Gamepad => !!pad?.connected,
      );
      const pad = pads.find((p) => padAction(p)) ?? pads[0];
      const next = pad
        ? controllerFamily(pad.id) + ":" + pad.mapping
        : "disconnected";
      if (next !== signature) {
        signature = next;
        setController({
          connected: !!pad,
          family: pad ? controllerFamily(pad.id) : "generic",
          standard: pad?.mapping === "standard",
        });
        repeater.step(undefined, now);
      }
      const foreground =
        document.visibilityState === "visible" && document.hasFocus();
      const action = repeater.step(
        pad && foreground ? padAction(pad) : undefined,
        now,
      );
      if (action) controllerAction(action, () => callback.current());
      frame = requestAnimationFrame(poll);
    }
    frame = requestAnimationFrame(poll);
    return () => cancelAnimationFrame(frame);
  }, []);
  return controller;
}
