$input v_coordinates, v_dimensions, v_shader_values, v_shader_values1, v_position, v_gradient_pos, v_gradient_pos2, v_gradient_texture_pos

// Copyright © 2026 Owlet Records. This file is part of Feathers.
// Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
// later version.

// **A SPARK'S SHAPE, ITS COLOUR AND ITS FADE — backlog HR.** The vertex stage moved it; this decides what it looks
// like when it gets there. Both are functions of the SAME per-quad floats plus the clock, so neither stage holds any
// state:
//
//     v_shader_values.xy   velocity at birth, pixels per second
//     v_shader_values.z    birth time, seconds
//     v_shader_values.w    life, seconds
//     v_shader_values1.x   this spark's seed, 0..1, forwarded by the vertex stage from `a_position.z`
//     v_shader_values1.y   the SPLIT: a signed child kick, pixels per second, 0 = never splits — the
//                          sixth and last per-quad float (`a_position.w`), backlog HR round 2
//
// **THE INK IS A UNIFORM RATHER THAN THE BRUSH, AND THAT IS WHY THERE ARE TWO `visage::Shader` OBJECTS.** A quad's
// brush reaches a shader through the gradient ATLAS — a texture lookup quantised to 8 bits a channel — which is fine
// for a colour and useless for anything a shader must compute with. The two spark colours are two constants, so the
// cheaper arrangement is one uniform and two shader objects over one compiled program: a `Shader*` is the batch id,
// so that is two batches, and two draw calls for the whole layer is nothing.

#include <shader_include.sh>

// rgb: the ink. w: how much of the brightness twinkles, 0 = none.
uniform vec4 u_spark_ink;
// x: now, seconds. y: drag. z: birth size in pixels. w: death size in pixels.
uniform vec4 u_spark_now;
// x: alpha at birth. y: glitter rate, radians per second. z: the RING's hole, as a fraction of the radius
// — a reversed grain's onset spark is hollow (`kSparkOnsetReverseHole`); 0 = filled.
uniform vec4 u_spark_look;
// **THE SPLIT'S SHAPE — `PerformPad::sparkChildOffsetAt` is the C++ half and they are edited together.**
// x: the angle the child leaves at, radians off the heading. y: seconds for the child lobe to grow.
// z: the child's radius as a share of the parent's round radius. w: the neck, the smooth-min width.
uniform vec4 u_spark_split;
// x, y: the earliest and latest split, as fractions of the life; linear in the seed between them.
uniform vec4 u_spark_split_at;
// **THE SHAPE'S FOUR NUMBERS — every one of them a feel constant.** x: the longest a spark gets, in units of its own
// round size, along its velocity. y: how far through its life it is longest, 0..1. z: the speed at which it streaks
// fully, pixels per second. w: the narrowest the across-axis may get, as a fraction of the round size.
uniform vec4 u_spark_shape;

// A CHEAP HASH — the standard sin-fract one. It needs only to decorrelate one spark's seed into several independent
// decisions, and the seed is already a well-spread 0..1.
float sparkHash(float p) {
  return fract(sin(p * 91.3458) * 47453.5453);
}

// A POLYNOMIAL SMOOTH MINIMUM — the union of two distance fields with a rounded neck `k` wide between them.
// It is below `min(a, b)` by at most `k/4`, which is the margin `PerformPad::sparkQuadSide` reserves.
float sparkSmoothMin(float a, float b, float k) {
  float h = clamp(0.5 + 0.5 * (b - a) / max(1.0e-4, k), 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}

void main() {
  float age = max(0.0, u_spark_now.x - v_shader_values.z);
  float life = max(1.0e-4, v_shader_values.w);
  // **PAST ITS LIFE IT IS GONE, and this is the frame the CPU has not retired it in yet.** The CPU retires on the
  // WORST-CASE life, because that is what makes the dead a prefix and the tick O(1); this is where the spark's own
  // shorter life is enforced, exactly and for free.
  if (age >= life)
    discard;

  float remain = clamp(1.0 - age / life, 0.0, 1.0);
  float through = 1.0 - remain;

  // **THE SHAPE EVOLVES OVER THE SPARK'S LIFETIME.** A round hot point at
  // birth, drawn OUT into a streak along the direction it is flying, thinning as it dies. The quad is reserved at the
  // largest extent (`vs_spark.sc`), so this only ever draws INSIDE it; what changes is the ellipse, not the rectangle.
  //
  // **ALONG THE VELOCITY, WHICH IS A CONSTANT DIRECTION UNDER ISOTROPIC DRAG.** `v' = -k*v` scales the speed and never
  // turns it, so the birth velocity's direction IS the current direction at every age and the heading needs no closed
  // form. A spark from a still puck has only its random kick, which is never exactly zero — the guard is here because
  // `normalize` of a zero vector is not.
  vec2 vel = v_shader_values.xy;
  float speed = length(vel);
  vec2 dir = speed > 1.0e-4 ? vel / speed : vec2(1.0, 0.0);
  vec2 perp = vec2(-dir.y, dir.x);

  // **HOW MUCH IT STREAKS IS HOW FAST IT IS GOING**, so a spark flung off a hard drag draws out and one that fell off
  // a resting puck stays round. That is the momentum feature made visible a second way, and it costs one more
  // `length` of a vector this stage already has.
  float haste = clamp(speed / max(1.0e-4, u_spark_shape.z), 0.0, 1.0);
  // AND WHEN: round at birth, longest at `u_spark_shape.y` through the life, easing back after. `smoothstep` either
  // side so neither end snaps.
  float peak = clamp(u_spark_shape.y, 0.01, 0.99);
  float envelope = through < peak ? smoothstep(0.0, peak, through)
                                  : 1.0 - 0.45 * smoothstep(peak, 1.0, through);
  float stretch = 1.0 + (u_spark_shape.x - 1.0) * haste * envelope;

  // **THE SIZE RAMP IS THE SHAPE'S NOW, NOT THE QUAD'S.** A spark still shrinks toward death (`kSparkSizeDeath`); it
  // does so by drawing a smaller ellipse rather than by being handed a smaller rectangle, because the rectangle has to
  // stay big enough for the streak.
  float sizeRamp = (u_spark_now.w + (u_spark_now.z - u_spark_now.w) * remain) / max(1.0e-4, u_spark_now.z);

  // **THE ELLIPSE, IN PIXELS FROM THE PARENT'S CENTRE — not in the quad's +/-1 coordinates any more.** The quad
  // used to be exactly `u_spark_shape.x` birth sizes across, so quad units were a fixed multiple of the spark; a
  // SPLITTING spark's quad is larger (it must hold the child lobe), so the shape is measured in pixels and sized off
  // `u_spark_now.z` instead. For a spark that never splits the two are the same arithmetic.
  vec2 pix = v_coordinates * v_dimensions * 0.5;
  float base = sizeRamp * u_spark_now.z * 0.5;
  float along = base * stretch;
  // **ACROSS NARROWS AS ALONG GROWS, AND IT IS FLOORED.** A needle thinner than a pixel stops being drawn at all,
  // which reads as the spark vanishing early rather than as a shape.
  float across = max(base * u_spark_shape.w, base / max(1.0, sqrt(stretch)));

  vec2 q = vec2(dot(pix, dir) / max(1.0e-4, along),
                dot(pix, perp) / max(1.0e-4, across));
  float d = length(q);

  // **THE SPLIT — a single spark with a special, growing shape.** After its split time a lobe swells out
  // of the spark's side and drifts off on its own kick, under the same drag; the smooth minimum draws the neck
  // between them while they are close, so it reads as a spark budding in two rather than as a second one appearing.
  // Everything is a closed form of the per-quad floats and the clock — `PerformPad::sparkChildOffsetAt` is the same
  // lines in C++, asserted there.
  float kick = v_shader_values1.y;
  if (kick != 0.0) {
    float seed = v_shader_values1.x;
    float ts = life * (u_spark_split_at.x + (u_spark_split_at.y - u_spark_split_at.x) * seed);
    float tau = age - ts;
    if (tau > 0.0) {
      float k = max(1.0e-4, u_spark_now.y);
      float side = kick > 0.0 ? 1.0 : -1.0;
      vec2 heading = cos(u_spark_split.x) * dir + side * sin(u_spark_split.x) * perp;
      vec2 child = heading * abs(kick) * ((1.0 - exp(-k * tau)) / k);
      float grown = smoothstep(0.0, 1.0, tau / max(1.0e-4, u_spark_split.y));
      float lobe = base * u_spark_split.z * grown;
      if (lobe > 1.0e-3) {
        d = sparkSmoothMin(d, length(pix - child) / lobe, u_spark_split.w);
      }
    }
  }

  // **A SOFT EDGE, which is a change from the CPU reference's hard square and is the point of being in a fragment
  // shader at all.** The reference is a known-good for the TRAJECTORY, not a pixel target.
  float shape = 1.0 - smoothstep(0.7, 1.0, d);
  // **A REVERSED GRAIN'S SPARK IS A RING** — its middle cut out, softly, to `u_spark_look.z` of the radius.
  if (u_spark_look.z > 0.0)
    shape *= smoothstep(u_spark_look.z * 0.6, u_spark_look.z, d);
  if (shape <= 0.0)
    discard;

  // **THE GLITTER: a function of (seed, age), so its properties live purely on the GPU.** The seed is a
  // per-quad float of its own since the shape work — hashing the velocity, as the first version did, ties every shape
  // decision to the direction the spark happens to be flying, which is the correlation a random number exists to
  // break. Defaults OFF so the GPU path and the CPU reference stay comparable frame to frame.
  float seed = v_shader_values1.x;
  float twinkle = 1.0 - u_spark_ink.w * (0.5 + 0.5 * sin(sparkHash(seed) * 6.2831853 + age * u_spark_look.y));

  // **NOT PREMULTIPLIED.** `BlendMode::Alpha` is `SRC_ALPHA, INV_SRC_ALPHA` for the colour channels
  // (`shape_batcher.cpp`'s `blendStateValue`), so the blender does the multiply and a shader that did it too would
  // square the alpha — a spark half-faded would be a quarter drawn.
  float alpha = remain * u_spark_look.x * twinkle * shape;
  gl_FragColor = vec4(u_spark_ink.rgb, alpha);
}
