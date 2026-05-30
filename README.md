# Libpo32

A drum synthesizer module for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move, powered by the libpo32 engine.

16 synthesis voices mapped to the General MIDI drum layout (MIDI notes 36–51). Three bundled kits included. Save your own kits directly from the module.

Demo: https://www.youtube.com/watch?v=blKvmuEcvjQ

## Features

- 16 independently programmable synthesis voices (GM drum layout: Kick, Snare, Closed HH, etc.)
- Per-voice controls: Wave, Pitch, Decay, Mod, Bend, Noise, Dist, Level, Noise Filter, Noise Envelope
- Role-aware randomizer — each pad randomizes within constraints appropriate for its sound type (kicks stay kick-like, hats stay hat-like, etc.)
- Per-pad randomize as well as full-kit randomize
- Full project state save/restore — customised sounds survive set saves and device reboots
- Global Decay scale and Level
- Save kits to device storage — new kits appear immediately, no reload needed
- Three bundled kits: tonic (full kit), tape (lo-fi), acid (808-style)
- `[Swap module...]` action for quick module switching

## Install

### Manual install

1. Download the latest `po32-drum-module.tar.gz` from [Releases](https://github.com/mestela/schwung-libpo32/releases)
2. Extract and copy to your Move:

```bash
tar -xzf po32-drum-module.tar.gz
scp -r po32-drum ableton@move.local:/data/UserData/schwung/modules/sound_generators/
```

3. Restart Schwung on the device (or power-cycle the Move).

## Building from source

Requires Docker (for cross-compilation to ARM64).

```bash
./scripts/build.sh    # builds dist/po32-drum-module.tar.gz
```

## Usage

Load **Libpo32** as the synth in a Schwung Signal Chain slot. MIDI notes 36–51 trigger voices 1–16 following the General MIDI drum map.

### Voice layout (GM drum map)

| Pad | MIDI | Name       | Pad | MIDI | Name        |
|-----|------|------------|-----|------|-------------|
| 1   | 36   | Kick       | 9   | 44   | Pedal HH    |
| 2   | 37   | Rim        | 10  | 45   | Mid Tom     |
| 3   | 38   | Snare      | 11  | 46   | Open HH     |
| 4   | 39   | Clap       | 12  | 47   | Lo-Mid Tom  |
| 5   | 40   | Snare 2    | 13  | 48   | Hi-Mid Tom  |
| 6   | 41   | Lo Tom     | 14  | 49   | Crash       |
| 7   | 42   | Closed HH  | 15  | 50   | Hi Tom      |
| 8   | 43   | Floor Tom  | 16  | 51   | Ride        |

### Per-voice parameters

Navigate to **Sounds → [pad name]** to edit a voice:

| Param  | Range           | Description |
|--------|-----------------|-------------|
| Wave   | Sine / Tri / Saw | Oscillator waveform |
| Pitch  | 0–100           | Oscillator frequency |
| Decay  | 0–100           | Oscillator envelope decay |
| Mod    | Drop / Sine / Noise | Pitch modulation mode |
| Bend   | 0–100           | Pitch modulation amount |
| Noise  | 0–100           | Oscillator/noise mix |
| Dist   | 0–100           | Distortion amount |
| Level  | 0–100           | Voice level |
| N.Filt | LP / BP / HP    | Noise generator filter mode |
| N.Env  | Exp / Lin / Clap | Noise envelope shape — **Clap** retriggeres the noise in rapid bursts before the main decay |
| Randomize | —            | Randomize this voice within its role constraints |

### Randomizer roles

Each pad is assigned a synthesis role which constrains what the randomizer produces:

| Role  | Pads          | Character |
|-------|---------------|-----------|
| Kick  | 1             | Low freq, decaying pitch, tonal |
| Snare | 3, 5          | Mid freq, oscillator + noise blend |
| Clap  | 4             | High-freq oscillator + noise, random pitch mod |
| Hat   | 7, 9, 11, 14, 16 | High freq, metallic/noisy, short-to-long decay |
| Tom   | 6, 8, 10, 12, 13, 15 | Mid freq, oscillator-dominant, pitched |
| Perc  | 2             | Wide open — anything goes |

### Noise Filter modes

- **LP** (low-pass) — warm, bassy noise; suits kicks and toms
- **BP** (band-pass) — focused mid-range; suits snare and clap
- **HP** (high-pass) — bright, airy noise; suits hi-hats and cymbals

### Noise Envelope modes

- **Exp** — natural exponential decay (default)
- **Lin** — linear decay with a gated tail
- **Clap** — rapid retrigger bursts before the main decay (requires Noise mix > 0)

## Kits

Bundled kits live in `src/kits/`. Each voice is a `.mtdrum` text file (Microtonic/Sonic Charge format). You can author kits by hand and drop them into a numbered subfolder.

Saved kits are stored at `/data/UserData/schwung/modules/sound_generators/po32-drum/presets/` on the device.

## Credits

Synthesis engine based on the [libpo32](https://teenage.engineering/products/po-32) engine by Teenage Engineering / Sonic Charge. Module implementation by mestela.
