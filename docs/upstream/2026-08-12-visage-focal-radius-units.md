# Upstream issue draft for visage: `Brush::radial`'s `focal_radius` is a fraction, and nothing says so

**Filed:** https://github.com/VitalAudio/visage/issues/101

**Repo:** github.com/VitalAudio/visage
**Size:** a doc comment, or a rename. No behaviour change needed.

---

## Summary

`Brush::radial`'s last parameter is a fraction of the radius, not a length — but it sits between two
parameters that ARE lengths, and carries a name that reads like one. Passing pixels there does not
fail; it silently inverts the gradient.

## Where

`visage_graphics/gradient.h`:

```cpp
static Brush radial(Gradient gradient, const Point& center, float radius_x, float radius_y,
                    Point focal_center, float focal_radius = 0.0f);
```

`radius_x` and `radius_y` are in the same units as the shape. `focal_radius` is not. The only
in-tree caller divides first — `visage_graphics/svg.h:86`:

```cpp
float f_radius = radius ? focal_radius / radius : 0.0f;
position = GradientPosition::radial(point1, radius, radius, point2, f_radius);
```

## What it cost us

We were porting a design whose glow is specified as a canvas radial gradient with an inner and an
outer radius in pixels, so we passed the inner radius straight through: about 6 against an outer of
about 200, where at most 1 belongs.

The result is not a warning or a clamp. The gradient comes out inverted — alpha 0 at the centre,
rising outward — so a soft glow around each object became a faint wash brightening toward the edge
of the scene. It shipped. Every unit test stayed green (the drawing was in the right place, in the
right colour, with the right blend mode, in the right order — nothing we had could see the SHAPE of a
gradient), and a user eventually reported that the glows were "absent".

## Suggested fix

Either document it at the declaration:

```cpp
/// `focal_radius` is a FRACTION of the radius (0..1), not a length — unlike radius_x/radius_y.
```

or rename it to something that cannot be read as a length (`focal_radius_ratio`,
`focal_radius_fraction`), which is what we would prefer, since the doc comment only helps a reader
who goes looking.

An assertion for values > 1 would also have caught ours immediately, if that fits the library's
style.
