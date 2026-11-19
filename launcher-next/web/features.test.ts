import { describe, it, expect } from "vitest";
import { featureEnabled, legacyFeatures } from "./features";
import { initialState } from "./backend";
describe("legacy feature availability", () => {
  it("keeps all 23 feature names explicit and distinguishes network status from configuration", () => {
    const state = initialState();
    expect(legacyFeatures).toHaveLength(23);
    expect(featureEnabled(state, "settings.network")).toBe(false);
    expect(featureEnabled(state, "unknown")).toBe(false);
    expect(
      legacyFeatures.filter((feature) => featureEnabled(state, feature)),
    ).toHaveLength(22);
    const provider = {
      capabilities: { network: true, networkConfiguration: false },
    } as NonNullable<typeof state.runtimeServices>;
    expect(
      featureEnabled(
        { ...state, runtimeServices: provider },
        "settings.network",
      ),
    ).toBe(false);
    provider.capabilities.networkConfiguration = true;
    expect(
      featureEnabled(
        { ...state, runtimeServices: provider },
        "settings.network",
      ),
    ).toBe(true);
  });
});
