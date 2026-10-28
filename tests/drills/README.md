# tests/drills/

Clean speech plus the files derived from it (16-bit PCM mono WAVs).

## What ships in the repo

| file                      | what it is                              |
|---------------------------|-----------------------------------------|
| `paint-drying-clean.wav`  | clean speech: "The Paint Drying Podcast," Episode 1, hosted by the fictional Gary Bland (script: `paint-drying-script.txt`) |
| `paint-drying-script.txt` | script for the clean file               |

Only the clean file ships. The noisy variants are generated locally by
`../derive-drills.py` (deterministic, seed 42, standard library only) on
the first test run, or by hand:

```sh
python3 ../derive-drills.py   # generates any missing derived files
```

## Derived files (generated, not committed)

| file                     | what it is                    | how it's used                              |
|--------------------------|-------------------------------|--------------------------------------------|
| `paint-drying-hiss.wav`  | speech + broadband hiss       | **gated**: >= 20 dB floor reduction, peaks within 1 dB |
| `paint-drying-hum.wav`   | speech + mains hum            | **gated**: >= 40 dB floor reduction with the hum module on (RNNoise alone manages ~28 dB, so a broken hum module fails) |
| `paint-drying-combo.wav` | speech + hiss + hum           | report-only: hum adds ~1 dB here, so no gate; the hum drill is the hum gate |
| `paint-drying-quiet.wav` | synthetic room tone (-60 dB white noise) | no-speech guard: floor must not rise; listen to tests/build/paint-drying-quiet-b128.wav -- it should sound steady and quiet, not swelling/breathing (pumping) or chirpy artifacts |

Existing derived files are left alone when the suite runs. To regenerate
one, delete it (or pass `--force` to the derive script) and re-run.

## Using your own audio

Replace `paint-drying-clean.wav` with your own clean speech (16-bit PCM
mono WAV; convert with `ffmpeg -i in.wav -ac 1 -ar 48000 -sample_fmt s16
clean.wav` if needed), delete the derived files, and re-run the suite --
the derive script regenerates them from your file. The gates still apply:
hiss >= 20 dB, hum >= 40 dB with the hum module on, peaks within 1 dB.

## Do NOT substitute synthetic test tones

A pure sine is tonal content and RNNoise is designed to preserve tonal
content, so a tone will measure "no reduction" even when the model is
working perfectly. Tones belong in unit tests of the hum filter alone,
with the model out of the path -- not here.
