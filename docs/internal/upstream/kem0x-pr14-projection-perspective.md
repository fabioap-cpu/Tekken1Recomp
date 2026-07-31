# PR #14 projection precision and perspective textures

Source: [kem0x PR #14](https://github.com/mstan/psxrecomp/pull/14), commit
[`ec889d6e5956fc4f34acfee41d87f30b24123b7f`](https://github.com/mstan/psxrecomp/commit/ec889d6e5956fc4f34acfee41d87f30b24123b7f).

This branch parks two coupled, opt-in visual experiments together:

- retain discarded GTE 16.16 screen fractions and use exact packed-coordinate
  matches for supersampled software-renderer vertices;
- track exact SWC2 RAM provenance through generated, strict, dirty-RAM,
  overlay, and fallback interpreters so textured world polygons can use
  perspective-correct UV interpolation without guessing from screen position.

They share the same projection side cache, GP0 triangle preparation, and
software-renderer consumption path; splitting those internals would duplicate
most of the risk-bearing machinery. Both remain disabled until their existing
generic setters are called.

Excluded: all Web host exports and every title-specific state, action, address,
or presentation hook from the source commit. The separate 60 Hz presentation
experiment is parked on its own branch. Authorship is retained in the commit
trailer.
