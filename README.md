# Eglantine-NX2 — community Switch 2 fork

Community fork of **Eglantine** (itself a fork of the Eden Switch emulator) with extra Nintendo Switch 2
("NX2") compatibility work. It adds support for **Orbitals** (Unreal Engine 5) and **Metroid Prime 4:
Beyond (Switch 2 Edition)** on top of the existing Switch 2 titles. Bug reports, fixes and pull
requests are welcome: that is the point of this repository.

> **Legal notice.** This repository contains emulator source code only. It does **not** contain, and
> will never accept, game files (ROMs/dumps), `prod.keys`, `title.keys`, firmware or any other
> copyrighted or console-specific material. You must dump your own games, keys and firmware from
> hardware you own. This project is not affiliated with or endorsed by Nintendo.

## Download

Prebuilt Windows x64 build: see the **[Releases](../../releases)** page (`eglantine.exe`, built from the
source of the matching tag). Requirements: a Vulkan-capable NVIDIA GPU (tested on an RTX 4070 Ti), and
your own keys/firmware in `%APPDATA%\eden\keys` as with any Eden-based emulator. Add your game folder
(exefs + romfs dump) in the emulator and launch the entry of the **update** when there is one.

## Status

| Game | State |
|---|---|
| Metroid Prime 4 (Switch 2 Edition) | boots, title screen, save, intro, gameplay with HUD, 60 FPS, sound. About 18 minutes of play in TV mode without a crash. Not checked past the first area. |
| Orbitals | boots, intro movie, menus, character select, sound, handheld and docked modes (docked reaches the menus). Runs at 30 FPS by the game's own design. Some shaders may still be rejected in later levels. |
| Others (Mario Kart World, Splatoon Raiders, Pokopia, ...) | whatever the base Eglantine supports; see `NX2_NOTES.md`. |

Known gaps: Orbitals past the character select has had little testing; flickering was reported in later
parts of the game before the last shader fixes; Joy-Con 2 mouse sensor (Jcms) is stubbed.

## What this fork adds on top of Eglantine (`216b5034c6`)

- Switch 2 hardware audio codec (`hwopus` command 100, `nn::codec::hwacod`) decoded with libopus.
- `audsmx` output streams (`nn::audio::OutputStream`) played on the host audio device.
- `vi2` frame pacing: release fence in the present reply and `GetRecordedTimestamps` (command 53).
- `bsd` poll fix (a spin loop that used a full CPU core), QMD release slot 52, `fermi_2d` slice blits.
- Ampere to Maxwell shader lowering: `SHF`, `SUST`/`SULD`/`SURED`/`SUATOM` on 1D and 1D-buffer surfaces,
  `TXD` and `TLD4` with a uniform handle, better end-of-program detection when capturing shaders.
- Metroid Prime 4 specific handling (Switch 2 Edition mode, RomFS mounting, boot fix).

Details and debugging notes: `NX2_NOTES.md`.

## Building

See `docs/Build.md`. On Windows: Visual Studio Build Tools (C++), CMake, Ninja and the Vulkan SDK
(for `glslangValidator`). A typical configuration:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON -DYUZU_CMD=OFF ^
  -DYUZU_TESTS=OFF -DYUZU_ROOM=OFF -DENABLE_WEB_SERVICE=OFF -DENABLE_UPDATE_CHECKER=OFF ^
  -DENABLE_WERROR=OFF -DCMAKE_INTERMEDIATE_DIR_STRATEGY=SHORT
cmake --build build
```

Tips: use a short build path (MSVC fails with error C1041 on very long ones), and set `VSLANG=1033`
when your Visual Studio is not in English, otherwise Ninja does not track header dependencies.

## Reporting a bug

Open an issue with: the game and update version, your GPU and driver, what you did, and the log
(`%APPDATA%\eden\log\eden_log.txt`; it can contain folder names from your PC, so check it before
posting). Screenshots help for graphics bugs. Never attach keys, dumps or ROMs.

## Credits and license

Based on the work of the Eden, yuzu and Citra teams, the Eglantine authors and the PokeDinner fork by
Franseven (`git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork`). This repository keeps the original
history and licenses (GPL-3.0-or-later, see `LICENSE.txt` and `LICENSES/`). Binaries are distributed
with the corresponding source in this repository.

---

# Français (résumé)

Fork communautaire d'Eglantine (basé sur l'émulateur Eden) avec le support de la Switch 2 pour
**Orbitals** et **Metroid Prime 4 (Switch 2 Edition)**. Le dépôt ne contient que du code : **aucune ROM,
aucune clé, aucun firmware** ; dumpe toi-même tes jeux et tes clés depuis ta console. Le build Windows
est dans l'onglet **Releases**. Les bugs et les corrections sont les bienvenus via les issues et les
pull requests.

---

<!--
# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later
-->
<!-- lang: en-GB -->
<h1 align="center">
  <br>
  <a href="https://git.eden-emu.dev/eden-emu/eden"><img src="https://github.com/user-attachments/assets/35031845-4bfa-4916-8bf5-bc3bce67d5e2" alt="Eglantine" width="200"></a>
  <br>
  <b>Eglantine</b>
  <br>
</h1>

<h4 align="center"><b>Eglantine</b> is a free and opensource (FOSS) Switch 2 emulator, derived from Eden, Yuzu and Sudachi - started by developer Camille LaVey.
It's written in C++ with portability in mind, with builds for Windows, Linux and macOS
</h4>

<p align="center">
    </a>
    <a href="https://discord.gg/jCCZ58VExk">
        <img src="https://img.shields.io/discord/1367654015269339267?color=5865F2&label=Eglantine&logo=discord&logoColor=white"
            alt="Discord">
    </a>
  
</p>

<p align="center">
  <a href="#compatibility">Compatibility</a> |
  <a href="#development">Development</a> |
  <a href="#building">Building</a> |
  <a href="#download">Download</a> |
  <a href="#support">Support</a> |
  <a href="#license">License</a>
</p>

## Compatibility

The emulator is capable of running Mario Kart World at decent/full speed depending on specs and configurations, and Splatoon Raiders at variable performances

A list of supported games will be available in future. Please be patient.

## Development
For development discussions, please join us on [Discord](https://discord.gg/jCCZ58VExk)

If you would like to contribute, we are open to new developers and pull requests. Please ensure that your work is of a high standard and properly documented. You can also contact any of the developers on Discord or Stoat to learn more about the current state of the emulator.

## Documentation

We have a user manual! See our [User Handbook](./docs/user/README.md).

## Building

See the [General Build Guide](docs/Build.md)

For information on provided development tooling, see the [Tools directory](./tools)

## Download

You can download the latest releases from [here](https://github.com/Djihads80/Eglantine/releases).

## Support

We currently do not take in any donations for the time being

If you would prefer to support us in a different way, please join our [Discord](https://discord.gg/jCCZ58VExk)

## License

Eglantine is licensed under the GPLv3 (or any later version). Refer to the [LICENSE.txt](https://github.com/Djihads80/Eglantine/blob/nx2/LICENSE.txt) file.
