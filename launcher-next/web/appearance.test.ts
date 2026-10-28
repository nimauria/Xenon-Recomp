import { describe, expect, it } from "vitest";
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { initialState } from "./backend";
import { backgrounds, palettes, resolveAppearance } from "./appearance";

describe("converted legacy appearance", () => {
  it("preserves all four original palettes and follows system appearance", () => {
    expect(palettes.map((p) => p.id).sort()).toEqual([
      "carbon",
      "industrial",
      "light",
      "xenon-dark",
    ]);
    expect(resolveAppearance(initialState(), true).id).toBe("xenon-dark");
    expect(resolveAppearance(initialState(), false).id).toBe("light");
    const state = initialState();
    state.settings = { "ui/theme": "carbon" };
    expect(resolveAppearance(state, false).palette.window).toBe("#0D1114");
  });
  it("retains original theme backgrounds rather than substituting placeholders", () => {
    let count = 0;
    for (const [theme, variants] of Object.entries(backgrounds))
      for (const variant of variants) {
        if (theme === "light") continue;
        const file = resolve(
          `public/theme-art/backgrounds/theme-${theme}-${variant}.png`,
        );
        expect(existsSync(file)).toBe(true);
        expect(readFileSync(file).subarray(1, 4).toString()).toBe("PNG");
        count++;
      }
    expect(count).toBe(7);
    for (const family of ["blue", "green", "amber"])
      for (const name of [
        "side_strip",
        "rail_vertical",
        "hud_arc_left",
        "corner_top_left",
        "corner_bottom_right",
        "slashes",
        "divider_long",
      ])
        expect(
          existsSync(
            resolve(`public/theme-art/decor/${family}/${name}_${family}.svg`),
          ),
        ).toBe(true);
  });
  it("scopes background variants to the selected theme", () => {
    const state = initialState();
    state.settings = {
      "ui/theme": "carbon",
      "appearance/backdropVariant/carbon": "hud",
    };
    expect(resolveAppearance(state, true).variant).toBe("nebula");
    state.settings["ui/theme"] = "light";
    expect(resolveAppearance(state, true).image).toBe("");
  });
  it("applies accents, contrast, corners, transparency and motion together", () => {
    const state = initialState();
    state.settings = {
      "ui/theme": "industrial",
      "ui/accent": "violet",
      "ui/corners": "square",
      "appearance/panelOpacity": 0.5,
      "accessibility/reduceMotion": true,
    };
    const appearance = resolveAppearance(state, true);
    expect(appearance.accent).toBe("#A78BFA");
    expect(appearance.variables["--control-radius"]).toBe("4px");
    expect(appearance.variables["--panel-opacity"]).toBe("0.5");
    expect(appearance.reduceMotion).toBe(true);
    expect(resolveAppearance(state, true, true).enabled).toBe(false);
    expect(
      resolveAppearance(state, true, true).variables["--panel-opacity"],
    ).toBe("1");
  });
});
