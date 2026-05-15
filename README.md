# libpo32

A drum synthesizer module for [Schwung](https://github.com/charlesvestal/schwung) on Ableton Move, powered by the libpo32 engine.

16 synthesis voices, each independently programmable. Three bundled kits (tonic, tape, acid) included. Save your own kits directly from the module.

## Features

- 16 percussive synthesis voices per kit
- Per-voice controls: Wave (Sine/Tri/Saw), Pitch, Decay, Mod (Drop/Sine/Noise), Bend, Noise, Distortion, Level
- Global Decay scale and Level
- Save kits to device storage — new kits appear immediately, no reload needed
- Randomize all voices at once
- Three bundled kits: tonic (full kit), tape (lo-fi), acid (808-style)

## Install

### Via Module Store

Coming soon.

### Manual install

1. Download the latest `po32-drum-module.tar.gz` from [Releases](https://github.com/mestela/schwung-libpo32/releases)
2. Extract and copy to your Move:

```bash
tar -xzf po32-drum-module.tar.gz
scp -r po32-drum ableton@move.local:/data/UserData/schwung/modules/sound_generators/
```

3. Restart Schwung on the device.

## Building from source

Requires Docker (for cross-compilation to ARM64).

```bash
./scripts/build.sh    # builds dist/po32-drum-module.tar.gz
./scripts/install.sh  # deploys to Move at move.local
```

## Usage

Load `libpo32` as the synth in a Schwung Signal Chain slot. MIDI notes 36–51 trigger voices 1–16.

Navigate voices via **Sounds → P01–P16**. Each voice has:

| Param | Range | Description |
|-------|-------|-------------|
| Wave  | Sine/Tri/Saw | Oscillator waveform |
| Pitch | 0–100 | Oscillator frequency |
| Decay | 0–100 | Oscillator envelope decay |
| Mod   | Drop/Sine/Noise | Pitch modulation mode |
| Bend  | 0–100 | Modulation amount |
| Noise | 0–100 | Noise mix |
| Dist  | 0–100 | Distortion amount |
| Level | 0–100 | Voice level |

## Kits

Bundled kits live in `src/kits/`. Each voice is a `.mtdrum` text file — you can author kits by hand and drop them into a numbered subfolder.

Saved kits are stored at `/data/UserData/schwung/modules/sound_generators/po32-drum/kits/` on the device.

## Credits

Synthesis engine based on the libpo32 engine. Module implementation by mestela.
