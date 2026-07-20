# Maytera Chess - third-party asset & code provenance

- **Chess engine, AI, renderer, UI**: original code written for MayteraOS
  (GPLv2, same as the OS tree).
- **Piece models**: generated **procedurally** in `render.c` (surfaces of
  revolution + detail solids). No third-party 3D model files are used, so there
  are no external model licenses to honor.
- **Overlay font** (`font8x8.h`): `font8x8_basic` by Daniel Hepper,
  **Public Domain** (also shipped with the MayteraOS TinyGL port in
  `libgl/src/font8x8_basic.h`).
- **TinyGL** (`libgl`): software OpenGL 1.x, already part of the OS tree
  (see its own LICENSE); Maytera Chess only links against it.
- **AI-generated art** delivered to `/CHESS/*.BMP` (board / marble / backdrop /
  logo) is produced by the OS maintainer's image pipeline and folded into the
  boot image separately; the game loads it at runtime with graceful fallback.
