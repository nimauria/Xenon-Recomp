#include <array>
#include <cassert>
#include <iostream>

#include "xenon/gpu/blend_constant.hpp"

using xenon::gpu::BlendFactor;
using xenon::gpu::BlendState;
using xenon::gpu::shared_blend_factor_constant;

namespace {

constexpr std::array<float, 4> kConstant{0.1f, 0.2f, 0.3f, 0.75f};

BlendState blend(BlendFactor source, BlendFactor destination, bool enabled = true) {
  BlendState state{};
  state.color_source = source;
  state.color_destination = destination;
  state.alpha_source = BlendFactor::One;
  state.alpha_destination = BlendFactor::Zero;
  state.enabled = enabled;
  return state;
}

// Regression: D3D12 maps guest CONSTANT_ALPHA to BLEND_FACTOR, which reads the
// constant's RGB. The guest constant was passed through, so a colour slot
// fading by the constant's alpha blended by its red, green and blue instead.
void test_constant_alpha_broadcasts_alpha() {
  const std::array<BlendState, 1> blends{
      blend(BlendFactor::ConstantAlpha, BlendFactor::InvConstantAlpha)};
  const auto host = shared_blend_factor_constant(blends, kConstant);
  assert(host.exact);
  for (const float channel : host.value) assert(channel == 0.75f);
  std::cout << "  [PASS] constant alpha in a colour slot sets (a, a, a, a)\n";
}

void test_constant_colour_and_other_factors_pass_through() {
  const std::array<BlendState, 2> colour{
      blend(BlendFactor::ConstantColor, BlendFactor::InvConstantColor),
      blend(BlendFactor::SrcAlpha, BlendFactor::InvSrcAlpha)};
  auto host = shared_blend_factor_constant(colour, kConstant);
  assert(host.exact && host.value == kConstant);

  // Constant alpha only in the alpha equation: BLEND_FACTOR's alpha is right.
  BlendState alpha_only = blend(BlendFactor::One, BlendFactor::Zero);
  alpha_only.alpha_source = BlendFactor::ConstantAlpha;
  host = shared_blend_factor_constant(std::array<BlendState, 1>{alpha_only}, kConstant);
  assert(host.exact && host.value == kConstant);

  // A disabled slot's factors are not used.
  const std::array<BlendState, 1> disabled{
      blend(BlendFactor::ConstantAlpha, BlendFactor::Zero, false)};
  host = shared_blend_factor_constant(disabled, kConstant);
  assert(host.exact && host.value == kConstant);
  std::cout << "  [PASS] constant colour, alpha-equation and disabled slots keep the constant\n";
}

void test_both_constant_factors_are_reported() {
  const std::array<BlendState, 2> both{
      blend(BlendFactor::ConstantAlpha, BlendFactor::Zero),
      blend(BlendFactor::One, BlendFactor::ConstantColor)};
  const auto host = shared_blend_factor_constant(both, kConstant);
  assert(!host.exact && host.value == kConstant);
  std::cout << "  [PASS] constant colour and alpha together are reported as inexact\n";
}

}  // namespace

int main() {
  std::cout << "Shared blend-factor constant tests\n";
  test_constant_alpha_broadcasts_alpha();
  test_constant_colour_and_other_factors_pass_through();
  test_both_constant_factors_are_reported();
  return 0;
}
