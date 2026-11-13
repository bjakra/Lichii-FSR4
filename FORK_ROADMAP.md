# Lichii-FSR4 roadmap

Fork of [zortos293/Lichii](https://github.com/zortos293/Lichii). End goal: FSR 4 (including the
INT8 model for RDNA 2/3) running in most 3D games from the game's real inputs (depth, motion,
jitter), with the game rendering fewer pixels so it runs faster. FSR 3.1 is the fallback
everywhere; XeSS joins later as another backend.

Status: draft. Phases 0 and 1 are being researched now; later phases will be revised as the
earlier ones show what the games actually expose.

## Where upstream stands

| Game | Today |
|---|---|
| Ocarina of Time | Game's upscale pass replaced by DLSS (Windows), FSR 3.1, or FSR 4. Motion is reconstructed from depth and camera matrices, jitter is added by the host. |
| Fire Emblem: Fortune's Weave | Spatial de-jitter by default. Temporal replacement (DLSS on Windows, or FSR 3.1) from the game's own motion, depth and jitter only with `EDEN_NX2_DLSS_REPLACEMENT`; no setting for it. No FSR 4. |
| The Duskbloods network test | Game's DLSS replaced by FSR 3.1 by default when depth is captured. No FSR 4. |
| Star Fox | Spatial stand-in only, opt-in by environment variable. |
| Fast Fusion | DLSS recognised but left native: input conventions unproven. |
| Everything else (Mario Kart World included) | No upscaler replacement. |

FSR 4 on Linux goes through the community `fsr4-vk` port and has not been tested on AMD hardware
upstream.

## Phase 0: foundation

- Fork, local Linux build, `fsr4-vk` built locally.
- Prove FSR 4 INT8 runs on an RX 6800 XT (RADV) in Ocarina of Time; fix what breaks.
- Port the two fixes without which FSR 4 never starts on RADV: the buffer-device-address check
  (zortos293/Lichii#1540) and the descriptor id `fsr4-vk` accepts (zortos293/Lichii#1595).
- Fall back to FSR 3.1 when FSR 4 outputs black or NaN, and retry after a failed start (from
  CheezyEmu `3260bc5470`, `f2f236e148`).
- INT8 model: `fsr4-vk` stays pinned to AMD's FSR 4.1.1 binaries. Its six INT8 weight banks are
  byte-identical in AMD's newest release (FidelityFX SDK 2.4.0, FSR 4.1.2), so RDNA 2/3 already
  gets the newest full INT8 network. `fsr4-vk` cannot extract from the 2.4.0 binaries yet.

Done when: Ocarina logs `Ocarina FSR 4 evaluated` on RDNA 2 and the image is correct.

## Phase 1: one backend chain

- A common temporal-upscaler interface: colour, depth, motion, jitter, exposure, masks in; one
  output image out. FSR 4, FSR 3.1 and DLSS implement it.
- One user preference (FSR 4 → FSR 3.1 → the game's own pass), used by every path that replaces
  an upscaler, with the fallback reason in the log.
- FSR 4 for the generic game-DLSS path (Fire Emblem, The Duskbloods network test). The FSR 4
  upscaler is built around Ocarina's LDR inputs today and needs HDR colour, reversed infinite
  depth, auto exposure and a motion scale.
- A per-game setting for Fire Emblem's temporal replacement, replacing the environment variable.
- Settings UI, status bar and overlay name the upscaler that is actually running.

Done when: every game that has temporal replacement today can run FSR 4, and a failure falls
back without a restart.

## Phase 2: more games that ship DLSS

These games already produce everything a temporal upscaler needs, so this is the cheapest way to
cover more titles with real inputs.

- Capture tooling that records a game's DLSS evaluation: resources, formats, constants, jitter.
- Prove the input conventions per game (motion direction and scale, depth convention, jitter
  units, reset) and add its contract. Fast Fusion first.
- Investigate recognising the console's DLSS evaluation generically instead of by per-game shader
  hash. Open question: whether the evaluation is identifiable and its inputs are laid out the same
  way across games.

Done when: adding a DLSS game is a contract entry plus a capture, not new renderer code.

## Phase 3: render fewer pixels

Replacing an upscaler changes quality, not speed. Speed comes from lowering the render size.

- Quality modes (Quality / Balanced / Performance) that lower the game's render size, as upstream
  already does for Ocarina with DLSS, extended to FSR 4 and FSR 3.1.
- Per game: either patch the game's fixed render scale or steer its dynamic resolution.
- Measure GPU time per game and mode. FSR 4 INT8 has a real cost on RDNA 2, and a CPU-bound game
  gains nothing; the UI should say when a mode will not help.

Done when: a Performance mode measurably raises the frame rate in at least one GPU-bound game.

## Phase 4: games without DLSS

Mario Kart World and most other titles. There is no evaluation to replace, so the upscaler has to
be inserted. Ocarina's path is the template.

- Find the scene: colour and depth targets at scene size, and the point before HUD and
  post-processing where the upscaler belongs.
- Motion: use the game's velocity buffer where one exists; otherwise reproject depth through the
  current and previous camera matrices. Camera-only motion ghosts on moving objects, so skinned or
  moving geometry needs extra work per game.
- Jitter: offset scene viewports by a sub-pixel sequence, as Ocarina's path does.
- Texture LOD bias and HUD compositing at output size.
- A per-game profile format holding what the heuristics cannot find (matrix location, pass
  identity, exclusions), so supporting a game is data plus validation.

Done when: Mario Kart World runs FSR 4 below native render size with a stable image, and a second
game works from a profile alone.

## Phase 5: breadth

- Profiles for the most-played 3D titles, with a documented way to contribute one.
- XeSS backend on Windows (XeSS 3 has a Vulkan backend and runs on non-Intel GPUs). Intel ships
  no Linux runtime, so Linux gets it only if that changes. Downloaded on request, never bundled.
- Windows parity for everything above through AMD's official runtime.
- Regular merges from upstream.

## Limits

- "Most 3D games" means games with a profile or a recognised DLSS evaluation, not all of them
  automatically.
- Reconstructed motion is never as good as a real velocity buffer.
- No AMD or Intel binaries ship with the fork; they are built or downloaded on the user's machine.
