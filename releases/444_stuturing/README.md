# Stuturing — Stuttering on Turing Machine

A clock-quantized beat slicer, repeater, and granular time-stretcher driven by a **Music Thing Turing Machine** shift register, with up to **8 seconds** of continuous recording in pure SRAM using slice-aligned **IMA-ADPCM** compression.

---

## Quick Start & How to Use

1. **Audio**: Connect your sound source (drum break, synth pattern, vocal line) to **Audio In 1**. Listen to the processed glitch output on **Audio Out 1** (or dry pass-through on **Audio Out 2**).
2. **Clock**: Send a master clock pulse to **Pulse In 1**.
3. **Select Mode with the Toggle Switch**:
   * **MID (Live Stutter)**: Slices, repeats, and mangles incoming audio continuously in real time.
   * **DOWN (Quantized Record)**: Hold the momentary switch down to capture a beat-locked loop (up to 8 seconds). Release to seal the loop.
   * **UP (Frippertronics Freeze)**: Locks the recorded buffer and enables sound-on-sound tape overdub with soft saturation and high-frequency tape damping. Knob X controls loop decay time.
4. **Mangle with the Knobs**:
   * Turn the **Main Knob** through 8 combination zones to mix Shuffle, Repeat, and Time-Stretch.
   * Turn **Knob X** to control generative Turing Machine mutation (Live mode) or tape loop decay (Freeze mode).
   * Turn **Knob Y** to change clock subdivisions ($\div 1$ to $\div 16$).

---

## Hardware Interface

### Controls Summary

| Control | Type | Function |
|---|---|---|
| **Toggle Switch** | 3-way (Mom-Off-On) | DOWN = Quantized Record, MID = Live Stutter, UP = Freeze / Frippertronics |
| **Main Knob** | Continuous (large) | 8 Effect Combination Zones (Clean, Shuffle, Repeat, Stretch, Combos) + Intensity |
| **Knob X** | Continuous (trim) | Turing Mutation Probability (Live) / Frippertronics Loop Decay (Freeze) |
| **Knob Y** | Continuous (trim) | Clock Subdivisions ($\div 1, \div 2, \div 3, \div 4, \div 6, \div 8, \div 16$) |

---

### Detailed Control Behavior

#### Toggle Switch (3 positions)
* **DOWN (Momentary) — Nearest-Beat Quantized Record**:
  Press down to arm or start recording. Recording quantizes to the nearest clock beat:
  * If pressed within the first 50% of a beat, recording starts retroactively from the beat that just passed.
  * If pressed after the first 50% of a beat, recording arms and starts on the next clock pulse.
  * When released within the first 50% of a beat, the loop closes immediately at the beat that just passed.
  * When released after the first 50% of a beat, the loop closes on the next clock pulse.
  * Loop length is always an integer number of clock beats (up to 8 seconds).
* **MID (Default) — Live Stutter / Sliding Window**:
  Continuously records incoming audio into a circular ring buffer while slicing, repeating, and time-stretching incoming material in real time.
* **UP (Latching) — Freeze / Frippertronics Tape Loop**:
  Locks the current audio window and enables continuous sound-on-sound overdub. New audio at `Audio In 1` is merged with the looping material through a gentle low-pass tape damping filter and soft saturation.
  * In Freeze mode, **Knob X** and **CV In 1** set loop decay length:
    * **~7 o'clock (min)**: 85% feedback (quick decay in 4 to 8 passes, dub delay style).
    * **~12 o'clock (center)**: 92% feedback (musical decay in 8 to 16 passes).
    * **~5 o'clock (max)**: 99.2% feedback (>80 passes, near-infinite sound-on-sound drone).

#### Main Knob — 8 Effect Combinations & Intensity
The large central knob navigates 8 discrete combination zones. The knob signal uses Exponential Moving Average (EMA) filtering and +/-24-count asymmetric deadband hysteresis to prevent zone flickering:
1. `Clean` (Dry pass-through)
2. `Shuffle` (Slices reordered by Turing register)
3. `Repeat` (Slice stutter / ratchets)
4. `Stretch` (Granular time-stretch & reverse)
5. `Shuffle + Repeat`
6. `Shuffle + Stretch`
7. `Repeat + Stretch`
8. `All 3 (Shuffle + Repeat + Stretch)`

* **Within each zone**: Position controls effect intensity from 0% to 100%. The last 20% of travel acts as a saturation plateau (clamped at 100%) to provide a stable buffer before entering the adjacent zone.
* **Granular Stretch & Reverse (Zones 4, 6, 7, 8)**:
  * **Lower travel (intensity < ~83%)**: Forward breakcore granular stretch ($1.0\times$ to $8.0\times$).
  * **Upper travel (intensity > ~83% / > 3400)**: Forced reverse stretch for backward break rolls.
  * **Turing Bit 3**: Automatically triggers reverse playback generatively based on the mutation rate.

#### Knob X — Turing Mutation / Frippertronics Decay (+ CV In 1)
* **Live Mode (Switch MID/DOWN)**: Sets Turing Machine shift register mutation probability:
  * **~12 o'clock**: 50% bit-flip probability (constant generative variation; changes every cycle).
  * **~9 or ~3 o'clock**: Slipping loop (mostly repeats with occasional mutations).
  * **~5 o'clock**: Locked deterministic repeating loop (0% mutation, fixed pattern).
  * **~7 o'clock**: Alternating double-length locked loop (100% bit inversion).
* **Freeze Mode (Switch UP)**: Directly controls loop persistence / decay time (85% to 99.2% feedback).

#### Knob Y — Ratchet Subdivisions
Selects slice division rate per master clock beat: $\div 1, \div 2, \div 3, \div 4, \div 6, \div 8, \div 16$.
* Turning Knob Y temporarily engages a 1.2-second LED gauge displaying the selected subdivision.

---

### Front-Panel LEDs

The 6 LEDs are arranged in two columns of three:
```
[LED 0] Top-L   |  [LED 1] Top-R
[LED 2] Mid-L   |  [LED 3] Mid-R
[LED 4] Bot-L   |  [LED 5] Bot-R
```

#### Left Column (System & Clock Status)
* **LED 0 (Top Left)**: Master clock indicator (dim flash on clock pulse, bright 100ms flash on loop boundary).
* **LED 2 (Mid Left)**: Recording & Freeze indicator (Off in Live mode, pulsing during quantized record, solid bright in Freeze mode).
* **LED 4 (Bottom Left)**: Turing Machine shift register activity monitor.

#### Right Column (Real-time Effect Monitor)
Each LED indicates real-time effect execution with three distinct states:
* **Off**: Effect not included in active combination zone.
* **Dim Glow (~20% brightness)**: Effect armed in active combination zone, awaiting trigger.
* **Full Brightness (100%)**: Effect actively processing the current slice.
* **LED 1 (Top Right)**: **Shuffle** (slice jump).
* **LED 3 (Mid Right)**: **Repeat** (stutter ratchet).
* **LED 5 (Bottom Right)**: **Stretch** (granular time-stretch / reverse).

#### Temporary Subdivision Gauge (Knob Y)
Rotating Knob Y temporarily overrides the display for ~1.2 seconds with a vertical progress ladder:
* **$\div 1$**: 1 LED lit (`LED 4` Bot-L)
* **$\div 2$**: 2 LEDs lit (`LED 4, 2` Bot-L, Mid-L)
* **$\div 3$**: 3 LEDs lit (`LED 4, 2, 0` Left column full)
* **$\div 4$**: 4 LEDs lit (`LED 4, 2, 0` + `LED 5` Bot-R)
* **$\div 6$**: 5 LEDs lit (`LED 4, 2, 0` + `LED 5, 3` Mid-R)
* **$\div 8$**: All 6 LEDs lit solid
* **$\div 16$**: All 6 LEDs rapidly flash in unison (~8 Hz turbo ratchet)

---

### Patch Jacks

| Jack | Type | Function |
|---|---|---|
| **Audio In 1** | Audio In | Main audio input (16-bit 48 kHz). |
| **Audio Out 1** | Audio Out | Processed Stuturing glitch / stutter / stretch output. |
| **Audio Out 2** | Audio Out | Always-dry pass-through of `Audio In 1`. |
| **Pulse In 1** | Gate In | Master clock input (rising edge triggers beat quantization). |
| **Pulse In 2** | Gate In | External stutter gate: forces continuous ratchets while HIGH (+5V). |
| **Pulse Out 1** | Trigger Out | Sub-slice trigger: emits a 2 ms +5V pulse at each slice boundary. |
| **Pulse Out 2** | Trigger Out | Buffered mirror of `Pulse In 1` master clock. |
| **CV In 1** | CV In | Bipolar CV added to Knob X (modulates mutation rate or loop decay). |
| **CV In 2** | CV In | Slice Scrubbing: manually scans through recorded slices across the window. |
| **CV Out 1** | Gate Out | Effect Gate: +5V when stutter, stretch, or shuffle is active; 0V when clean. |
| **CV Out 2** | CV Out | Slice decay ramp: descending linear ramp (+5V $\rightarrow$ 0V) across each slice. |

---

### Modular Patch Ideas

1. **Percussive Gating / Pluck**:
   Patch **CV Out 2** into the modulation input of a VCA or filter cutoff (e.g. Workshop System Ring Mod or Humpback filter) along with **Audio Out 1**. The descending ramp shapes each slice into a punchy, click-free decay pluck.
2. **Manual Slice Scrubbing**:
   Send an LFO, slow envelope, or sequencer CV into **CV In 2** (-2.5V to +2.5V). This overrides automatic slice selection and scrubs through slices sequentially across the recorded audio buffer.
3. **Performance Stutter Drops**:
   Connect a manual gate button or keyboard gate output to **Pulse In 2**. Pressing the gate forces instantaneous stutter ratchets at the Knob Y subdivision rate.

---

## Technical Specifications

* **Sampling Rate**: 48,000 Hz, mono.
* **Buffer Memory**: 192 KB pure SRAM ring buffer ($384\,000$ 4-bit samples $\approx 8.0$ seconds).
* **Audio Codec**: Slice-aligned IMA-ADPCM with 1st-order error-feedback noise shaping.
* **Seeking & Latency**: 64-sample dense keyframe grid with microsecond seek time.
* **Anti-Click Conditioning**: 16-sample C0 linear crossfade on slice boundaries and external clock resync.
* **Switch Debounce**: 20 ms integrating state machine lock.
* **Processor Clock**: 200 MHz RP2040 core.

---

## Build

To compile the firmware and generate the UF2 file:

```sh
make BUILD_DIR=/tmp/build_stuturing build
```

The resulting UF2 file is copied to:
[releases/444_stuturing/UF2/stuturing.uf2](releases/444_stuturing/UF2/stuturing.uf2)
