$input v_coordinates, v_dimensions, v_shader_values, v_position, v_gradient_pos, v_gradient_pos2, v_gradient_texture_pos

// Copyright © 2026 Owlet Records. This file is part of Feathers.
// Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
// License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
// later version.

// THE TERRAIN AS A PLANET: its light, multiplied over the map (backlog FE). The quad is the chart disk's bounding
// square, so v_coordinates is the disk in radii, -1..1 each way, and a point there is read orthographically as a point
// on a sphere with normal (x, y, sqrt(1 - r^2)).
//
// **THE LIGHT IS FAR AWAY, AND THE STAR MARKS WHERE IT STANDS OVERHEAD**: the light is far from the atmosphere, and
// the star is the point through which a ray normal to the planet surface, towards the true light, passes. So the
// star is not a lamp at a height: it is the
// subsolar point, and the light is the single direction the normal there points in. Every point is then lit by
// `dot(normal, direction)` — full at the star, falling to nothing at 90 degrees from it, which is a terminator across
// the sphere rather than a bright patch with an edge. IT REPLACES THE LAMP: a lamp in low orbit lit a spot, one raised
// to 60% of the radius was better, and at infinity there is nothing left to tune but the softness below.
//
// `softness` stands for the light's angular size: a star of no width would cut the terminator as a hard line.
// Outside the disk the factor is 1 — the rim mask drawn after owns that ground.

#include <shader_include.sh>

uniform vec4 u_planet_light; // xy: the subsolar point, in disk radii (y down); z: terminator softness; w: ambience 0..1
uniform vec4 u_planet_tint;  // rgb: the light's colour; a: unused

void main() {
  vec2 p = v_coordinates;
  float r2 = dot(p, p);
  float inside = step(r2, 1.0);
  vec3 n = vec3(p, sqrt(max(0.0, 1.0 - r2)));

  vec2 q = u_planet_light.xy;
  float q2 = dot(q, q);
  q = q2 > 1.0 ? q / sqrt(q2) : q; // a star off the chart stands over the horizon
  vec3 toLight = vec3(q, sqrt(max(0.0, 1.0 - dot(q, q))));

  // **THE ANGLE OF INCIDENCE IS THE LIGHT**: with the light centred, the sphere must still dim towards its edges,
  // because the rays there are glancing. This line was wrong: a `smoothstep` alone SATURATES, so everything more
  // than `softness` from the terminator came out at
  // full brightness and the lit face was a flat wash. Lambert's cosine is the falloff — a ray meeting the ground at a
  // glance spreads its light over more of it — and the smoothstep is kept only as the terminator's own softness, which
  // is what a light of some angular size does to that line.
  float softness = max(u_planet_light.z, 1e-3);
  float incidence = dot(n, toLight);
  float lit = max(incidence, 0.0) * smoothstep(-softness, softness, incidence);

  float ambience = u_planet_light.w * (0.55 + 0.45 * n.z);
  vec3 light = vec3(ambience, ambience, ambience) + (1.0 - u_planet_light.w) * lit * u_planet_tint.rgb;

  // **NORMALISED SO THE FULLY LIT POINT MULTIPLIES BY ONE**: the shade was too milky, but switching it off
  // wholesale lost the darkening at the edges — the falloff is wanted, not the price it was charging for it.
  //
  // WHERE THE MILK CAME FROM. This is a MULTIPLY over the map, and at full illumination it was
  // `ambience + (1 - ambience) * tint` — which is 1 only on the hue's brightest channel and less on
  // every other. So even the point facing the light straight on had the ground dimmed and pulled
  // toward the light's colour, and the whole interior paid it. MEASURED radially, mean grey of a 20px
  // box out of 255: the shading cost the middle 11.7 levels and r/R 0.54 a further 33.9, while the
  // ATMOSPHERE at the middle contributed +0.01 — a hundredth of a level. The air was never the
  // problem; taking it away, which is what was asked for first, would have changed nothing visible.
  //
  // DIVIDING BY THAT PEAK COSTS THE FALLOFF NOTHING, which is the point: `lit` still runs 1 down to 0
  // across the terminator, so the limb still darkens to `ambience` and the sphere still reads as a
  // sphere. Only the exposure moves — the brightest point is now exactly unchanged ground instead of
  // ground seen through a filter.
  vec3 peak = vec3(u_planet_light.w, u_planet_light.w, u_planet_light.w)
              + (1.0 - u_planet_light.w) * u_planet_tint.rgb;
  light = light / max(peak, vec3(1e-3, 1e-3, 1e-3));
  light = min(light, vec3(1.0, 1.0, 1.0));
  gl_FragColor = vec4(mix(vec3(1.0, 1.0, 1.0), light, inside), 1.0);
}
