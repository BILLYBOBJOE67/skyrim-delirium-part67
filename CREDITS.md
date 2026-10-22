# Credits and licences

Skyrim Delirium is free software under the GNU General Public License, version 3 only (GPL-3.0-only). See `LICENSE`.
Its source, and the source of the Delirium engine it ships, must be available to everyone who gets the mod.

## Delirium

The physics and behaviour engine (`delirium_physics.dll`) is [Delirium](https://github.com/PVCPC/delirium-physics) by PVCPC,
GPL-3.0-only. This mod ships a build of a fork with Skyrim SDK changes (per-part external torques and joint state for
FOLLOW hosts); that fork's source is published with each release.

## Game data

This mod ships no GTA IV data. Delirium reads the character it needs from the player's own copy of GTA IV at startup.
Nothing in this repository is extracted from GTA IV or derived from its files: data read from local game installs lives in
`local/`, which is never committed or shipped.

## Third-party code (all GPL-3 compatible)

- [CommonLibSSE-NG](https://github.com/CharmedBaryon/CommonLibSSE-NG): MIT
- [SKSE64](https://skse.silverlock.org/): the plugin loads under it; not shipped
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444): required at runtime; not shipped
- [spdlog](https://github.com/gabime/spdlog), [fmt](https://github.com/fmtlib/fmt): MIT
- [SimpleIni](https://github.com/brofield/simpleini): MIT
- [xbyak](https://github.com/herumi/xbyak): BSD-3-Clause
- [rapidcsv](https://github.com/d99kris/rapidcsv): BSD-3-Clause
- [DirectXMath](https://github.com/microsoft/DirectXMath), [DirectXTK](https://github.com/microsoft/DirectXTK): MIT
