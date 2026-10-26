[简体中文](README.md) | **English**

![Yosuga no Sora + Haruka na Sora](docs/images/banner.jpg)

# Yosuga no Sora + Haruka na Sora for macOS

A native macOS port of the Simplified Chinese editions of *Yosuga no Sora* (ヨスガノソラ) and its fan disc *Haruka na Sora* (ハルカナソラ). No Wine, no virtual machine: the engine is the SDL3 port of the official EntisGLS4 SDK, running the original Cotopha scripts natively on Apple silicon. Both works live in one app; press 「ハルカナソラ」 on Yosuga no Sora's title screen to enter Haruka na Sora.

> This is a private archive repository. The games are © Sphere; the Simplified Chinese translation belongs to the Sphere 中文化委员会. The repository contains source code and tools only — no game data; building requires your own copies. Please support the original release (see the [disclaimer](#disclaimer)).

## Download and installation

No ready-made app is published (it would contain the complete games). Build it from your own game folders:

1. On an Apple silicon Mac (tested on macOS 27) with the Xcode command line tools and Homebrew:

   ```bash
   brew install cmake bison webp
   python3 -m pip install pillow numpy fonttools
   ```

2. Get the source and build (the game folders are only read, never modified):

   ```bash
   git clone https://github.com/007xf/Yosuga-no-Sora-macOS.git
   cd Yosuga-no-Sora-macOS/source
   ./build.sh "/path/to/缘之空" "/path/to/悠之空"
   ```

   The first run makes the 4x HD set from the games' own pixels and takes a few hours; later builds take minutes. The interface font 圆体-简 (Yuanti SC) is a macOS downloadable font and is fetched automatically.

3. Drag `source/build/stage/缘之空.app` into Applications.

If macOS says the developer cannot be verified, choose *Open Anyway* in System Settings → Privacy & Security, or run:

```bash
xattr -dr com.apple.quarantine "/Applications/缘之空.app"
```

The app is about 9.9 GB (HD art 5.9 GB, game data 3.9 GB). Saves live in `~/Library/Application Support/Sphere/YosugaNoSora/` and survive deleting the app.

## Screenshots

| | |
|:---:|:---:|
| ![Title](docs/images/01-title.jpg) Title | ![Dialogue](docs/images/02-dialogue.jpg) Dialogue |
| ![Choice](docs/images/03-choice.jpg) Choice | ![Save](docs/images/04-save.jpg) Save |
| ![Settings](docs/images/05-config.jpg) Settings | ![Gallery](docs/images/08-gallery.jpg) Gallery |
| ![Route map](docs/images/06-route-map.jpg) Route map | ![Route overview](docs/images/07-route-overview.jpg) Route overview |
| ![Haruka na Sora](docs/images/09-haruka.jpg) Haruka na Sora title | ![History](docs/images/10-history.jpg) History |

Screenshots use test saves.

## What changed

### Native port

- Official EntisGLS4 SDK + SDL3; the original Cotopha scripts run unmodified, natively on Apple silicon.
- The original 4:3 picture; in full screen the sides get black bars — never cropped or stretched.
- macOS menus and shortcuts: ⌘S save, ⌘L load, ⇧⌘S/⇧⌘L quick save/load, A auto, H hide the text window, ⌘Y history, ⌘→ skip, ⌃⌘F full screen; ⌘, opens the native settings.

### The 空澄 interface

- Every screen redesigned in the games' bright summer-sky colours: the dialogue window keeps the original teal-to-sky gradient, refined into clear glass; save, load, settings, confirmations, choices and the gallery are frosted white glass with deep-teal text.
- All interface lettering is drawn at 4x with rounded faces (圆体 and SF Rounded) — no more jagged upscaled labels.
- The gallery is rebuilt with the original character portraits and the track list typeset in a rounded Japanese face.

### HD text and art

- Dialogue, names, choices and save details are lettered again at 4x density inside the engine (same face, border and shadow), sharp on Retina displays, with the layout identical to the original.
- 4,225 CGs, sprites, backgrounds and UI textures are 4x interpolations of the original pixels — no generative redrawing — each passing a strict fidelity review (alpha IoU ≥ 0.97, colour difference ≤ 10, SSIM ≥ 0.90).
- The HD art is stored as lossless WebP: pixel-identical to the reviewed PNGs at about half the size.

### Route map (攻略进度)

- A 攻略进度 entry on the title screen opens an in-game page modelled on the *WHITE ALBUM 2* port: branch flow, route overview and special content.
- Only choices actually reached and answers actually taken are recorded; you can return to a reached choice and decide again, or enter a chapter from its start. Unreached content stays hidden.

## Who did what: Claude and ChatGPT

- **Claude**: running Sphere's 2008 EntisGLS games on the SDL3 engine, the save/load screens, merging both works with the Haruka na Sora entry, the 4:3 window, packaging and the icon; in 1.4 the 空澄 interface, HD text, the in-game route map, the lossless WebP store, final acceptance and publishing.
- **ChatGPT (Codex)**: the native macOS shell (settings, menus, graceful exit), the faithful 4x HD pipeline and its review, choice font and mouse fixes, full-screen key fixes, and the route map's data and restore machinery (choice checkpoints, cross-work restore).
- They worked in relay through a handoff document: [`source/HANDOFF.md`](source/HANDOFF.md).

## Repository contents

| Path | Contents |
|---|---|
| `source/build.sh` | One-shot build: engine, original UI art, HD set, packaging |
| `source/vendor/EntisGLS-SDL3/` | The engine (SDL3 port of the EntisGLS4 SDK, with this port's patches) |
| `source/mods/src/` | The 空澄 interface: every UI texture drawn in code at 4x |
| `source/tools/` | Archive I/O, art export, HD pipeline, packaging and verification |
| `docs/images/` | Screenshots |

## Acknowledgements

- Sphere — the original games
- Sphere 中文化委员会 — the Simplified Chinese translation
- 理影 / Entis soft — EntisGLS
- nL6DHZc3RhMnC/EntisGLS-SDL3 — the SDL3 port of EntisGLS
- SDL, FreeType, libwebp

## Disclaimer

- For personal study and research only; no commercial use.
- The game content is © Sphere and the translation belongs to its translators; this project is not affiliated with them.
- The repository contains no game data; build it with copies you own.
- Takedown requests will be honoured. Use at your own risk.
