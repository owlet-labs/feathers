$input v_coordinates, v_dimensions, v_shader_values, v_position, v_gradient_pos, v_gradient_pos2, v_gradient_texture_pos

// Copyright © 2026 Owlet Records. This file is part of Feathers.
// Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
// later version.

// THE PLANET'S ATMOSPHERE, ADDED OVER THE MAP AND THE RIM (backlog FE) — and it is HOW MUCH AIR THE EYE LOOKS THROUGH
// rather than a glow paint. Its reflectivity is much less than the surface's, and its luminescence is greatest
// around the perimeter, where it is thickest — looking through the atmosphere into empty space. With the light
// centred there is almost no atmosphere above the surface.
//
// THE PATH THROUGH THE SHELL, WHICH IS THE WHOLE MODEL. The planet is the unit sphere and its air is the shell out to
// `1 + thickness`. A line of sight at distance r from the centre crosses that shell for
//     2 * (sqrt((1+t)^2 - r^2) - sqrt(1 - r^2))   inside the disk, where the ground cuts the path short,
//     2 * sqrt((1+t)^2 - r^2)                     outside it, where the path runs on into empty space,
// which is nearly nothing over the middle of the disk, longest exactly AT the limb, and falling to nothing by the outer
// edge. So a centred light shows almost no air, which is what the eye expects and what the first version got wrong by
// painting a ring whatever the geometry said.
//
// AND IT IS LIT LIKE THE GROUND, not merely present: the air along that path glows by how much of it faces the
// satellite, with a small floor so the night side keeps a trace of it rather than a hard edge. `reflectivity` is well
// under the ground's, because air scatters a little of the light and rock returns most of it.

#include <shader_include.sh>

// HOW MUCH OF THE ADDED LIGHT IS THE DOME'S EDGE rather than the air in front of the ground. Its own number because the
// two say different things: the air is a depth, and this is a surface catching light.
#define kSheenShare 0.22

uniform vec4 u_planet_light; // as the shade: xy the subsolar point in disk radii, z terminator softness
uniform vec4 u_planet_tint;  // rgb the light's colour
uniform vec4 u_planet_air;   // x reflectivity, y shell thickness in radii, z quad scale, w night floor

void main() {
  vec2 p = v_coordinates * u_planet_air.z;
  float r = length(p);
  float thickness = max(u_planet_air.y, 1e-3);
  float outer = 1.0 + thickness;

  // HOW MUCH AIR IS IN FRONT OF WHATEVER IS BEHIND IT, normalised by the longest path there is (at the limb).
  float beyond = sqrt(max(0.0, outer * outer - r * r));
  float ground = r < 1.0 ? sqrt(max(0.0, 1.0 - r * r)) : 0.0;
  float path = max(0.0, beyond - ground);
  float longest = sqrt(max(1e-6, outer * outer - 1.0));
  // **MEASURED AS THE AIR IN EXCESS OF LOOKING STRAIGHT DOWN, NOT AS THE WHOLE PATH** (2026-09-20:
  // with shade enabled even the centre of the frame looked washed out, and that was unintended).
  //
  // THE COMMENT ABOVE CLAIMED THIS WAS "nearly nothing over the middle of the disk" AND THE ARITHMETIC
  // DID NOT AGREE. Straight down through the shell the path is `thickness`, and normalised by the
  // limb's it is sqrt(t/(2+t)) — at the shipped t of 0.045 that is 0.148, so the very centre of the
  // chart was getting 15% of the limb's air depth. Added, with the star centred, that is about 4.5% of
  // the tint laid over the WHOLE disk, which on dark terrain is exactly the milky lift that was seen.
  // The model was right and the normalisation was wrong: every line of sight crosses the shell, so
  // dividing by the longest path can never reach zero anywhere on the disk.
  //
  // THE FLOOR IS THE STRAIGHT-DOWN PATH, so what is drawn is how much MORE air the eye looks through
  // than it would at the middle — zero at the centre by construction, 1 at the limb, and the halo
  // untouched. This is the original spec arriving at last: with the light centred,
  // there is almost no atmosphere above the surface.
  float floorPath = outer - 1.0;
  float depth = clamp((path - floorPath) / max(1e-6, longest - floorPath), 0.0, 1.0);
  // MEASURED ON XVFB AGAINST THE KNOWN-BAD, radially along the horizontal through the disk centre
  // (0 = the middle, 1 = the rim), mean grey of a 20px box out of 255:
  //     r/R    0.00    0.27    0.54    0.77    0.90    0.98
  //     was  125.19  122.39  108.13   79.31   15.01    3.15
  //     now  115.66  112.29   98.43   72.15   13.94    3.15
  // The interior loses about 9.5 levels and THE RIM IS UNCHANGED TO THE LAST DIGIT, which is the
  // shape the fix claims: both formulations are 1 at the limb by construction, and they differ only
  // in how fast they fall away from it.

  // WHERE THAT AIR STANDS, and how much of it the satellite lights. THE AIR IS SAMPLED IN THE MIDDLE OF THE PATH THE EYE
  // LOOKS THROUGH, not on the ground under it — that air is ABOVE the surface and partly faces the viewer, so a star in
  // the middle of the disk still lights the limb and the halo around the edges survives. Sampling it at the surface would put the limb exactly edge-on and switch the ring off.
  vec2 q = u_planet_light.xy;
  float q2 = dot(q, q);
  q = q2 > 1.0 ? q / sqrt(q2) : q;
  vec3 toLight = vec3(q, sqrt(max(0.0, 1.0 - dot(q, q)))); // the subsolar point IS the direction: the light is far away
  vec2 outward = r > 1e-4 ? p / r : vec2(0.0, 0.0);
  float middle = 0.5 * (beyond + ground);
  vec3 air = normalize(vec3(outward * min(r, outer), middle) + vec3(0.0, 0.0, 1e-5));
  float softness = max(u_planet_light.z, 1e-3);
  float lit = smoothstep(-softness, softness, dot(air, toLight));

  float glow = depth * (u_planet_air.w + (1.0 - u_planet_air.w) * lit);
  // AND THE EDGE OF THE BUBBLE CATCHES THE LIGHT (the model: a flat sheet with a hemispherical bubble in
  // it): a tighter, brighter band than the air itself. IT USED TO BE `lit * lit`, WHICH IS WHY IT COULD NOT BE SEEN —
  // with the star near the middle of the disk the limb is edge-on to the light, so squaring an already small number put
  // the sheen under the noise. It follows the light plainly now, over a floor that keeps the bubble's edge drawn even
  // on the night side, which is what says "there is a dome here" when the light says nothing.
  float sheen = pow(depth, 3.0) * (0.35 + 0.65 * lit);
  float amount = u_planet_air.x * glow + kSheenShare * sheen;
  float scale = u_color_mult.x / 64.0; // the layer's colour range, without the gradient atlas' normalisation
  gl_FragColor = vec4(u_planet_tint.rgb * amount * scale, 1.0);
}
