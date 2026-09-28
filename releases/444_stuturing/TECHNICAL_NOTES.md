# Stuturing — Technical Notes & Architecture

This document details the engineering principles, memory layout, math constraints, and design tradeoffs implemented in **Stuturing** (Stuttering on Turing Machine) for the **Music Thing Modular Workshop Computer** (Raspberry Pi RP2040).

For user interface, panel layout, and patching instructions, see [README.md](README.md).

---

## 1. System Architecture & Resource Allocation

### 1.1 Memory Budget & SRAM Constraints
The Raspberry Pi RP2040 features 264 KB of internal SRAM split into 4 striped banks (256 KB) and 2 single-cycle scratchpad banks (4 KB each).

Stuturing operates entirely in SRAM to guarantee deterministic, zero-latency streaming without the stalls, wear leveling, and flash-erase blocking inherent to external QSPI flash memory:
* **Audio Sampling Rate**: 48,000 Hz, mono, 16-bit effective resolution.
* **Audio Buffer Length**: 8.0 seconds ($384\,000$ audio samples).
* **Compression**: IMA-ADPCM 4-bit per sample (compression ratio 4:1).
* **Buffer Footprint**: 
  $$\text{Memory} = \frac{384\,000 \times 4\text{ bits}}{8\text{ bits/byte}} = 192\,000\text{ bytes (192 KB)}$$
* **Remaining SRAM**: $\approx 72\text{ KB}$, allocated to the execution stack, interrupt handlers, `ComputerCard` peripherals buffers, and keyframe tables.

---

## 2. Audio Codec: Slice-Aligned IMA-ADPCM

### 2.1 The Differential Seeking Problem
The Interactive Multimedia Association (IMA) ADPCM algorithm is an adaptive differential pulse-code modulation scheme. The decoding of sample $n$ depends on the predictor $\hat{X}_{n-1}$ and the step-index $I_{n-1}$ of sample $n-1$:

$$\text{predictor}_n = \text{clamp}_{16}\left(\text{predictor}_{n-1} \pm \Delta(\text{nybble}_n, \text{step}_n)\right)$$
$$\text{step\_index}_n = \text{clamp}_{0..88}\left(\text{step\_index}_{n-1} + \text{IndexTable}[\text{nybble}_n]\right)$$

Jumping arbitrarily to a non-zero byte in an ADPCM stream without knowing the decoder state produces DC offsets, severe transient thumps, and gradual step divergence.

### 2.2 Slice Alignment & Keyframes
To enable instantaneous, non-sequential jumping across slices (Shuffle, Stutter, Repeats):
1. **Keyframe Storage**: During recording, when sample counter $pos$ aligns with a slice boundary ($pos \equiv 0 \pmod{L_{\text{slice}}}$), a compact `SliceKeyframe` is logged:
   ```cpp
   struct SliceKeyframe {
       int32_t sample_offset;
       adpcm_state_t enc_state; // { int16_t predictor; int8_t step_index; }
       adpcm_state_t dec_state;
   };
   ```
   Each keyframe requires only 6 bytes. A budget of 256 keyframes uses just 1.5 KB.
2. **Instant Restoration**: When the playback engine redirects reading to slice $K$, the decoder state is restored immediately:
   $$\text{read\_dec\_state} = \text{keyframes}[K].\text{dec\_state}$$
   $$\text{read\_sample\_pos} = K \times L_{\text{slice}}$$
3. **Boundary Anti-Click Micro-Fade**: A 64-sample symmetric linear ramp is applied at slice boundaries to suppress mechanical and phase discontinuities:
   $$\text{fade}(s, p, L) = \begin{cases}
     s \times \frac{p}{64} & 0 \le p < 64 \\
     s \times \frac{L - 1 - p}{64} & L - 64 \le p < L \\
     s & \text{otherwise}
   \end{cases}$$

### 2.3 1st-Order Noise Shaping at Encoding
IMA-ADPCM compresses 16-bit audio down to 4 bits per sample (16 levels of step delta). This quantization process generates uniform quantization noise across the spectrum (perceived as background "hiss").

To improve perceived signal-to-noise ratio (SNR) without increasing bit depth or computational overhead, Stuturing introduces **1st-order error-feedback noise shaping** in `adpcm_encode_shaped()`:

1. **Error Calculation**:
   At sample step $n$, the quantization error $e_n$ is the difference between the modified input sample $x'_n$ and the reconstructed quantized predictor $\hat{x}_n$:
   $$e_n = x'_n - \hat{x}_n$$
2. **Spectral Shaping Transfer Function**:
   The residual error is fed back into the next input sample with a slightly dampened feedback coefficient ($\beta = 7/8 = 0.875$, computed via shift-subtraction `e - (e >> 3)` for stability and zero float overhead):
   $$x'_{n+1} = \text{clamp}_{16}\left(x_{n+1} + \beta \cdot e_n\right)$$
   In the $z$-domain, the noise transfer function (NTF) acts as a high-pass filter on the quantization error:
   $$H(z) = 1 - \beta z^{-1}$$
3. **Acoustic Benefit**:
   * Attenuates quantization noise in the mid-range frequencies ($500\text{ Hz} \text{ to } 4\text{ kHz}$) where the human ear is most sensitive (Fletcher-Munson curves).
   * Pushes the noise energy upward towards the Nyquist frequency ($24\text{ kHz}$ at $48\text{ kHz}$ sample rate).
   * Cost: Only ~5 integer clock cycles per sample on the Cortex-M0+, with error accumulator reset at slice boundaries to maintain slice-alignment isolation.

---

## 3. Turing Machine Engine

The sequence generator is based on Tom Whitwell's classic **Music Thing Modular Turing Machine** 16-bit shift register.

### 3.1 Mathematical Formulation
Let $R \in [0, 2^{16}-1]$ be the shift register and $p \in [0, 4095]$ the smoothed and CV2-modulated Knob X value.

On every slice clock boundary:
1. Extract most significant bit: $\text{MSB} = (R \gg 15) \ \& \ 1$.
2. Determine inversion probability $P_{\text{invert}}$ based on pot deflection:
   * **Centre ($\approx 2048$, 12h)**: $P_{\text{invert}} \approx 50\%$. Register mutates continuously into novel random loops.
   * **Clockwise ($\to 4095$, 5h)**: $P_{\text{invert}} \to 0\%$. Register recirculates deterministically without mutation (Locked Loop).
   * **Counter-Clockwise ($\to 0$, 7h)**: $P_{\text{invert}} \to 100\%$. Register inverts the recirculated bit, yielding an alternating $2 \times 16 = 32$-step loop.
3. Update register:
   $$R_{t+1} = \left((R_t \ll 1) \mid (\text{invert} \oplus \text{MSB})\right) \ \& \ \text{0xFFFF}$$

### 3.2 Slice Routing, Independent Polyphony & Bit Mapping
Bits from $R_t$ are tapped independently to control real-time subslice decisions, allowing effects to trigger asynchronously across the sequence:
* **Repeat Trigger**: Bit 0 ($R_t \ \& \ 1$) or threshold comparator against zone intensity.
* **Shuffle Trigger**: Bit 1 ($(R_t \gg 1) \ \& \ 1$) or threshold comparator.
* **Stretch Trigger**: Bit 2 ($(R_t \gg 2) \ \& \ 1$) or threshold comparator.
* **Target Slice Address**: 4-bit nibble $(R_t \gg 4) \ \& \ \text{0xF}$, mapped modulo the active slice count:
  $$\text{target} = \left((R_t \gg 4) \ \& \ \text{0xF}\right) \pmod{N_{\text{slices}}}$$

When an effect is included in the active combo zone:
* If not triggered on the current subslice: its LED glows at ~5% brightness (armed / idle).
* When triggered on the current subslice: its LED flashes to full 100% brightness.
* When not included in the active combo zone: its LED remains completely dark (OFF).

---

## 4. Hardware Ergonomics & Analog Signal Conditioning

### 4.1 Switch Debouncing State Machine
The 3-position toggle switch (`Switch::Down` momentary, `Switch::Middle`, `Switch::Up` latching) exhibits mechanical contact bouncing on actuation and release.
* An integrating software debouncer requires a stable state for $960$ consecutive samples ($20.0\text{ ms}$ at $48\text{ kHz}$) before committing an edge.
* The recording state machine transitions:
  $$\text{IDLE} \xrightarrow[\text{SW Down}]{\text{debounced}} \text{ARMED\_WAIT} \xrightarrow{\text{PulseIn1} \uparrow} \text{RECORDING} \xrightarrow[\text{SW Released}]{\text{debounced}} \text{FINISHING} \xrightarrow{\text{PulseIn1} \uparrow} \text{IDLE / FROZEN}$$

### 4.2 Main Knob Physical Resolution & Hysteresis
Assigning the 8 combination zones to the **Main Knob** rather than Knob X takes advantage of its wide angular diameter and ergonomic leverage (large physical rotation per zone compared to a 9mm trim pot).
Partitioning 4096 counts into 8 discrete combination zones (512 counts per zone) while extracting continuous sub-zone parameter modulation can cause erratic boundary chatter due to ADC noise ($\approx 2\text{--}4\text{ LSB}$).

1. **Exponential Moving Average (EMA)**:
   $$y_n = y_{n-1} + \alpha (x_n - y_{n-1})$$
   Implemented with 24.8 fixed-point arithmetic ($\alpha = 16/256 = 1/16$), providing jitter rejection without noticeable latency.
2. **Asymmetric Deadband Hysteresis**:
   Transitioning between zone $Z$ and $Z \pm 1$ requires crossing a guard band:
   $$\text{Threshold}_{\text{up}} = (Z + 1) \times 512 + 24$$
   $$\text{Threshold}_{\text{down}} = Z \times 512 - 24$$
   This eliminates flickering when the potentiometer rests directly on an octave boundary.
3. **Knob X & CV In 2 Integration**:
   Moving the Turing mutation probability to Knob X frees `CV In 2` to modulate the mutation rate bipolarly, allowing external modular voltage to sweep between locked patterns and chaotic mutation.

---

## 5. Granular Time-Stretching & Dual Independent Decoder Architecture

### 5.1 The Grain Phase Overlap Distortion Problem
In time-domain overlap-add (TD-SOLA) stretching, two audio grains (Grain A and Grain B) read from different offsets within the slice simultaneously with a phase displacement (typically $180^\circ$ / half a grain length).

Because IMA-ADPCM relies on internal differential states (`predictor` and `step_index`), using a single shared decoder instance for both interleaved grain reads causes **severe state contamination**:
* Grain A updates the predictor according to sample $p_A$.
* The subsequent sample calculation reads Grain B at $p_B$ using Grain A's predictor.
* This introduces chaotic step-index divergence, severe harmonic distortion, and high-frequency hash.

### 5.2 Dual Independent Decoder States
To eliminate this harmonic degradation:
1. **Isolated State Trackers**:
   `GranularEngine` maintains two completely distinct `adpcm_state_t` instances: `dec_state_a_` and `dec_state_b_`.
2. **Grain Cycle Re-synchronization**:
   At the birth of each new grain window, the grain's dedicated decoder state is refreshed from the base slice keyframe (`last_base_state_`). As each grain advances sample-by-sample, its decoder state evolves in continuous phase with its own local reading trajectory.
3. **Interpolation & Overlap**:
   * Grain length: dynamically scalable from 512 to 1535 samples ($10.6\text{ to }32\text{ ms}$) modulated by `zone_intensity`.
   * Grains A and B are decoded via `AdpcmBuffer::DecodeAt(pos, window, &dec_state)` and crossfaded using complementary 8-bit triangular windows ($W_A + W_B = 256$):
     $$\text{output} = \frac{S_A \cdot W_A + S_B \cdot (256 - W_A)}{256}$$
* Zero floating-point math, zero runtime heap allocations, completely artifact-free granular stretching.

---

## 6. Software Architecture & Modularity

The code is strictly separated into focused, reusable modules following object-oriented embedded C++ best practices:

* **`adpcm.h`**: Stateless codec primitive with 1st-order error-feedback noise shaping (`adpcm_encode_shaped`, `adpcm_decode`).
* **`adpcm_buffer.h` (`AdpcmBuffer`)**: Encapsulates the 192 KB SRAM ring buffer, keyframe logging, boundary resets, and stateful sample retrieval.
* **`granular_engine.h` (`GranularEngine`)**: Implements dual-grain TD-SOLA synthesis with independent decoder state tracking.
* **`turing_engine.h` (`TuringEngine`)**: Manages the 16-bit shift register, probabilistic bit inversion, and parameter derivation.
* **`ui_controller.h` (`UIController`)**: Manages switch debouncing (20 ms lock), ADC exponential moving average (EMA) smoothing, and asymmetric deadband hysteresis.
* **`main.cpp` (`Stuturing`)**: Top-level coordinator binding audio IO, clock synchronisation, state transitions, and PWM LED diagnostics.
