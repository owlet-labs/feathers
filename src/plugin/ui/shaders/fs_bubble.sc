$input v_texture_uv

// THE BUBBLE (backlog FE): the terrain sheet wrapped onto the near face of a sphere.
//
// ONE LAW, AND IT IS `Bubble` IN celestial.h — the same radial map, written twice because it runs in
// two places for two reasons. C++ has it for the things that are PLACED (the star, the crumbs, the
// grid, the pointer's meaning); this has it for the ground itself, which is a picture and not a set of
// points. Where the two must agree is at their shared arithmetic, and the test that keeps them honest
// walks radii through both.
//
// AND IT RUNS BACKWARD, which is the thing to hold on to when reading it: a fragment shader is asked
// "what colour is THIS pixel", so it starts on the glass and looks for the ground under it. Hence
// asin here and sin there. Sampling forward — walking the sheet and scattering it onto the glass —
// would leave holes wherever the map expands, which near the middle it does by a factor of π/2.
//
// The flat map is the ordinary rendered layer: this effect samples the frame BELOW it, so everything
// upstream (the cubic upsample, the carried tiles, the prefetch) is untouched and does not know the
// ground is round.

#include <shader_include.sh>

SAMPLER2D(s_texture, 0);

uniform vec4 u_center_position;
uniform vec4 u_atlas_scale;
uniform vec4 u_dimensions;
uniform vec4 u_texture_clamp;

/// THE DISC, AS A FRACTION OF THIS REGION'S WIDTH — `.x`, and the rest is spare. It has to be told:
/// the frame is the whole stage rectangle, 866x534 at the shipped size, and the chart is a circle
/// inscribed in the SQUARE that fits inside it after the width column is reserved. Normalising each
/// axis by its own extent — the obvious reading, and what this shader did for one draft — measures
/// radius against an ELLIPSE 433 wide and 267 tall, which warps the ground anisotropically and reaches
/// a long way outside the chart. Neither shows: the rim mask paints over everything past the circle,
/// so the mistake is invisible in a photograph and in a difference against one. It is only visible
/// here, in the arithmetic, which is why the number is passed in rather than assumed.
uniform vec4 u_bubble;

#define kQuarterTurn 1.5707963267948966

void main() {
  // Texel offsets from the middle of the region, which is also the middle of the disc (`ExploreStage`
  // keeps the chart concentric with the frame), and the disc's radius in those same texels. One
  // length, one radius, no axes.
  vec2 px = (v_texture_uv / u_atlas_scale.xy) - u_center_position.xy;
  float radius = max(u_bubble.x * u_dimensions.x, 1.0);
  float glass = length(px) / radius;

  // OUTSIDE THE BALL THERE IS NO GROUND — and this frame paints the void the chart sits on as well as
  // the terrain, so the corner is passed through rather than thrown away. Returning nothing here would
  // punch the background out of the editor and leave the rim mask as the only thing covering it, which
  // is a lot of weight to put on a vignette. What the corner shows is what it always showed: flat
  // terrain, under the mask that hides it.
  if (glass > 1.0) {
    gl_FragColor = texture2D(s_texture, v_texture_uv);
    return;
  }

  float world = asin(min(glass, 1.0)) / kQuarterTurn;
  vec2 sheet = glass > 0.00001 ? px * (world / glass) : px;
  vec2 uv = (sheet + u_center_position.xy) * u_atlas_scale.xy;
  // The effect's own half-texel bounds: near the limb the map crushes a lot of sheet into very little
  // glass, so a pixel there asks for a coordinate close enough to the edge that a clamp is the
  // difference between the world's last row and whatever the atlas keeps next to it.
  uv = clamp(uv, u_texture_clamp.xy, u_texture_clamp.zw);
  gl_FragColor = texture2D(s_texture, uv);
}
