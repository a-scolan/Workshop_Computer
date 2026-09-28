# Stuturing — Stuttering on Turing Machine

A clock-quantized beat slicer, repeater, and granular time-stretcher driven by a **Music Thing Turing Machine** shift register, with up to **8 seconds** of continuous recording in pure SRAM using slice-aligned **IMA-ADPCM** compression.

---

## Hardware Interface

### Controls

* **Switch (3 positions)**:
  * **DOWN (Momentary)**: **Armed / Quantized Record**. Pressing down arms the window; recording starts exactly at the next rising edge on `Pulse In 1` and continues across successive clock beats until the switch is released and the subsequent clock beat arrives (quantized to exact integer beats, max 8 seconds).
  * **MID**: **Live Stutter / Sliding Window**. Continuous real-time recording into a circular buffer while slicing, repeating, and stretching incoming material.
  * **UP (Latching)**: **Freeze**. Locks writing into the buffer and indefinitely loops/stutters the captured window.
* **Main Knob — 8 Effect Combinations & Intensity**:
  The large central knob comfortably navigates 8 discrete combination zones, smoothed with an Exponential Moving Average (EMA) and stabilized by asymmetric deadband hysteresis to prevent ADC jitter flickering between zones:
  1. `Clean` (Direct pass-through)
  2. `Shuffle` (Slices reordered by Turing register)
  3. `Repeat` (Slice stuttering / ratchets)
  4. `Stretch` (Time-domain granular overlap-add stretching)
  5. `Shuffle + Repeat`
  6. `Shuffle + Stretch`
  7. `Repeat + Stretch`
  8. `All 3 (Shuffle + Repeat + Stretch)`
  *The position within each zone precisely modulates the internal parameter/intensity of the active effects.*
* **Knob X — Turing Machine Mutation (+ CV In 2)**:
  Controls the bit mutation rate of the shift register. Modulated bipolarly by `CV In 2`:
  * **~12 o'clock**: 50% bit-flip probability (continuous random generation, pattern changes every pass).
  * **~9 or ~3 o'clock**: Slipping loop (mostly repeats with infrequent mutations).
  * **~5 o'clock**: Locked deterministic repeating loop (0% mutation, stable seed).
  * **~7 o'clock**: Double-length locked loop (alternates every cycle).
* **Knob Y — Ratchet Subdivisions**:
  Selects slice division rate: $\div 1, \div 2, \div 3, \div 4, \div 6, \div 8, \div 16$ slices per clock beat.

---

### LEDs (Two columns of 3 LEDs)

* **LED 0 (Top Left)**: Master Clock pulse indicator (dim during clock pulse, **blip intense flash** on window start and completion).
* **LED 1 (Mid Left)**: Recording & Freeze indicator (Off in Live mode, Pulsing during quantized Record, Solid bright during Freeze).
* **LED 2 (Bottom Left)**: Turing Machine shift register activity monitor.
* **Right Column (LEDs 3, 4, 5)**: Real-time effect monitor (3 states per LED):
  * **Éteinte (OFF)** : Effet non sélectionné / désactivé dans le combo courant.
  * **Glow tamisé (200/4095, ~5 %)** : Effet armé dans le combo, en attente sur cette subslice.
  * **Pleine brillance (4000/4095)** : Effet déclenché et actif sur la subslice en cours de lecture.
  * **LED 3 (Haut-D)** : **Shuffle** (saut de tranche)
  * **LED 4 (Milieu-D)** : **Repeat** (bégaiement)
  * **LED 5 (Bas-D)** : **Stretch** (étirement granulaire)

---

### Patch Jacks

| Jack | Function |
|---|---|
| **Audio In 1** | Main audio input |
| **Audio Out 1** | Processed Stuturing output |
| **Audio Out 2** | Always dry live pass-through |
| **Pulse In 1** | Master clock input (rising edge) |
| **Pulse In 2** | External gate (forces stutter when HIGH) |
| **Pulse Out 1** | Sub-slice trigger (fires at each slice boundary) |
| **Pulse Out 2** | Mirror of `Pulse In 1` |
| **CV In 2** | Bipolar modulation added to Knob X (Turing mutation rate) |
| **CV Out 1** | Effect gate (+5V during stutter/stretch) |
| **CV Out 2** | Descending ramp across each slice (+5V $\rightarrow$ 0V) |

---

## Technical Details

* **Sample Rate**: 48 kHz mono.
* **Storage**: 192 KB SRAM ring buffer ($384\,000$ 4-bit samples $\approx 8$ seconds).
* **Slice-Aligned Keyframes**: Keyframes `{predictor, step_index}` are stored at each slice boundary to ensure zero-latency seeking without decoding latency.
* **Debouncing**: 20 ms state-machine software lock on the mechanical switch.
