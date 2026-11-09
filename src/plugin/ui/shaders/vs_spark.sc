$input a_position, a_color0, a_color1, a_color2, a_texcoord0, a_texcoord1, a_texcoord2
$output v_coordinates, v_dimensions, v_shader_values, v_shader_values1, v_position, v_gradient_pos, v_gradient_pos2, v_gradient_texture_pos

// Copyright © 2026 Owlet Records. This file is part of Feathers.
// Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
// later version.

// **A SPARK MOVES HERE AND NOWHERE ELSE — backlog HR: a spark's translation and its glitter properties are computed
// purely on the GPU.**
//
// The CPU writes a spark ONCE, at birth, and never touches it again. What it writes is this quad's rectangle — the
// position it was BORN at, sized to the largest it will ever be drawn — plus four floats of its own, which reach this
// stage as `a_texcoord2` (`Canvas::shader`'s `values`, a per-quad channel our visage fork adds):
//
//     a_texcoord2.x   vx, pixels per second at birth
//     a_texcoord2.y   vy
//     a_texcoord2.z   birth time, seconds, on `u_spark_now`'s clock
//     a_texcoord2.w   life, seconds
//
// **THE MOTION IS A CLOSED FORM, NOT AN INTEGRATION, WHICH IS THE WHOLE REASON THIS CAN LIVE ON THE GPU.** Under
// exponential drag `v' = -k·v` the displacement after `t` seconds is exactly
//
//     p(t) = p0 + v0 · (1 - e^(-k·t)) / k
//
// so a spark's position is a function of its own birth data and the clock — no previous frame, no state, no order.
// `PerformPad::sparkOffsetAt` is the same three lines in C++ and a case asserts THAT against an independent
// restatement; this stage and that function are the two halves of one formula and must be edited together.
//
// **AND THE SIZE IS THE QUAD SCALING ABOUT ITS OWN CENTRE.** The rectangle is fixed at the BIRTH size because a
// vertex shader may move vertices but the rasteriser only sees the triangle it is given; shrinking is therefore a
// scale of the corner offset. The centre is recovered from the corner coordinate, which is ±1 by construction
// (`setCornerCoordinates`), so `centre = a_position.xy - coord · dimension / 2`.

#include <shader_include.sh>

uniform vec4 u_bounds;
// x: now, seconds, the same clock `a_texcoord2.z` is stamped on. y: drag coefficient, per second.
// z: birth size in pixels. w: death size in pixels.
uniform vec4 u_spark_now;

void main() {
  vec2 mn = a_texcoord1.xy;
  vec2 mx = a_texcoord1.zw;

  vec2 dimensions = a_texcoord0.zw;
  vec2 corner = a_texcoord0.xy;
  // THE QUAD'S CENTRE, from the corner it is: the four coordinates are ±1, so this is the same point for all four.
  vec2 centre = a_position.xy - corner * dimensions * 0.5;

  float age = max(0.0, u_spark_now.x - a_texcoord2.z);
  float life = max(1.0e-4, a_texcoord2.w);
  // **CLAMPED TO THE LIFE so a spark that outlived its frame stops where it died rather than flying on.** The CPU
  // retires it on the next tick; this is what keeps the one frame in between honest.
  float t = min(age, life);

  float k = max(1.0e-4, u_spark_now.y);
  // (1 - e^(-k·t)) / k — the exact displacement integral for exponential drag.
  vec2 offset = a_texcoord2.xy * ((1.0 - exp(-k * t)) / k);

  // **THE QUAD IS THE SHAPE'S LARGEST EXTENT AT ANY AGE, AND IT DOES NOT CHANGE WITH AGE.**
  //
  // The CPU submits it at `kSparkSizeBirth · kSparkStreakMax` and this stage never scales it. That is a
  // decision against the obvious alternative — shrinking the quad as the spark shrinks — and the reason
  // is that the SHAPE now evolves inside the quad: a spark elongates into a streak part-way through its
  // life, so a quad sized for the round birth shape would clip the streak exactly when it appears.
  //
  // **A VERTEX SHADER MAY MOVE A QUAD OUT OF ITS SUBMITTED RECT — MEASURED, and it is what makes the
  // translation possible at all.** visage computes `clamp` from the CANVAS's clip region (the pad's
  // frame), not from the shape's rectangle: `setQuadPositions` writes `clamp_*` from `state_.clamp`.
  // So a spark flies freely inside the frame and is cut only at the frame's edge. What a vertex shader
  // cannot do is make a quad bigger than the rect the RASTERISER was given, which is why the extent is
  // reserved at submit time rather than grown here.
  float remain = clamp(1.0 - t / life, 0.0, 1.0);

  vec2 animated = centre + offset + corner * dimensions * 0.5;

  // **CLAMPED AFTER THE MOVE, NOT BEFORE IT.** The clamp is the canvas's clip region — the pad's own frame — so a
  // spark that has flown out of it must be cut at the frame's edge. Clamping the BIRTH position instead would pin
  // a spark to the edge it was born near and then move it out of the region unclipped.
  vec2 clamped = clamp(animated, mn, mx);
  vec2 delta = clamped - animated;

  v_position = animated;
  v_gradient_texture_pos = a_color0;
  v_gradient_pos = a_color1;
  v_gradient_pos2 = a_color2;
  v_dimensions = dimensions;
  v_coordinates = corner + 2.0 * delta / max(vec2(1.0e-4, 1.0e-4), dimensions);
  v_shader_values = a_texcoord2;
  // **THE SEED, FORWARDED BY HAND — and this is the asymmetry the fork patch's own comment warns about.**
  // The four floats of `a_texcoord2` are a declared VARYING and reach the fragment stage for free; the
  // two in `a_position.zw` are vertex-stage inputs only. `varying.def.sc` already declares a second
  // vec4 (`v_shader_values1`, TEXCOORD3) that nothing was using, so carrying it on costs a varying that
  // already existed rather than a change to the layout.
  // `.x` the seed, `.y` the SPLIT (the signed child kick, pixels per second — backlog HR round 2). The split moves
  // nothing here: the child lobe is drawn INSIDE this quad by the fragment stage, which is why the CPU sizes a
  // splitter's quad to hold it (`PerformPad::sparkQuadSide`).
  v_shader_values1 = vec4(a_position.z, a_position.w, 0.0, 0.0);

  vec2 adjusted_position = clamped * u_bounds.xy + u_bounds.zw;
  gl_Position = vec4(adjusted_position, 0.5, 1.0);
}
