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
* If not triggered on the current subslice: its LED glows at ~20% amplitude / 800 brightness (armed / idle).
* When triggered on the current subslice: its LED flashes to full brightness (4000).
* When not included in the active combo zone: its LED remains completely dark (OFF).

The 6 front-panel LEDs follow the standard ComputerCard physical pinout:
* **Left Column (System)**: LED 0 (Top-L: Clock), LED 2 (Mid-L: Rec/Freeze), LED 4 (Bot-L: Turing activity).
* **Right Column (Effects)**: LED 1 (Top-R: Shuffle), LED 3 (Mid-R: Repeat), LED 5 (Bot-R: Stretch).

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
3. **Knob X, CV In 1 & CV In 2 Integration**:
   Knob X controls the Turing mutation rate (and Frippertronics loop decay in Freeze mode), with `CV In 1` providing bipolar modulation. This frees `CV In 2` to act as a dedicated slice scrubbing and navigation input across the active audio window.
4. **Knob Y Dynamic Subdivision Metering**:
   Turning Knob Y triggers an instantaneous, non-blocking visual feedback mode on the 6 LEDs. When a parameter shift or zone change is detected on Knob Y, a 1.2-second countdown timer ($57\,600$ samples at $48\text{ kHz}$) is started. During this window, LEDs display a progressive vertical ladder:
   * Rank 1 ($\div 1$) to Rank 6 ($\div 8$): 1 to 6 LEDs illuminated from bottom to top (left column first, then right column).
   * Rank 7 ($\div 16$): All 6 LEDs pulse in unison at ~8 Hz to indicate maximum ratchet speed.
   * Once motionless for 1.2 seconds, the display seamlessly reverts to the live clock/Turing/effect monitor.

---

## 5. Granular Time-Stretching & Dual Independent Decoder Architecture

### 5.1 Analysis of 66_stretchcore & The Differential Codec Challenge
In dedicated breakcore hardware like `66_stretchcore`, granular time-stretching relies on:
* **Overclocking**: Running the RP2040 at 200 MHz (`set_sys_clock_khz(200000, true)`).
* **Breakcore Grain Sizing**: Dual-grain Overlap-Add (TD-SOLA) with large grains ($L = 2048$ samples $\approx 42.6\text{ ms}$ at 48 kHz) and a 50% hop interval ($H = 1024$ samples $\approx 21.3\text{ ms}$). This grain length avoids metallic comb-filtering whine and preserves punchy drum transients (kicks, snares).
* **Flat 0 dB Windowing**: Triangular windows summing to 1024 at every sample point ($W_A + W_B = 1024$), guaranteeing clickless, transparent crossfading.
* **Cubic Stretch Modulation**: Cubic knob easing to span smoothly from $1.0\times$ (subtle groove stretch) up to $8.0\times$ (deep jungle time-stretch).

However, while `66_stretchcore` streams uncompressed 8-bit signed PCM from pre-recorded Flash, Stuturing performs **live recording of incoming audio into a 4-bit IMA-ADPCM ring buffer in SRAM**.

Because IMA-ADPCM relies on internal differential states (`predictor` and `step_index`), naive jumps across grain offsets or resetting decoders to distant slice boundaries introduces severe step-index divergence, loud DC pops, and harsh bitcrush distortion.

### 5.2 Dense 64-Sample Keyframe Grid & Bounded Seeking
To guarantee bit-exact decoder synchronization at any arbitrary grain start position in the 8-second buffer:
1. **Dense Keyframe Grid**:
   A keyframe `adpcm_state_t` (int16_t predictor, int8_t step_index) is captured every 64 samples during encoding:
   $$\text{Grid Size} = \frac{384\,000\text{ samples}}{64\text{ samples/keyframe}} = 6000\text{ keyframes} \times 4\text{ bytes} = 24\text{ KB}$$
2. **Deterministic, Microsecond Seek Time**:
   To seek to any arbitrary position $P$ in the circular buffer, the engine looks up keyframe $\lfloor P / 64 \rfloor$ and decodes at most 63 differential nybbles forward:
   $$\text{Max Seek Time} \le 63 \times 35\text{ cycles} \approx 2200\text{ cycles} \approx 11\ \mu\text{s at 200 MHz}$$
   This ensures that every grain begins with **100% bit-perfect ADPCM state**.

### 5.3 Incremental Background Decoding & Zero-Glitch Reverse Playback
A key constraint of the RP2040 is that `ProcessSample()` executes inside a single-sample DMA interrupt period ($20.83\ \mu\text{s}$ at 48 kHz). Decoding all 2048 samples of a grain at once would take $\approx 350\ \mu\text{s}$, overflowing the DMA interrupt buffer.

To completely eliminate execution spikes:
1. **Ping-Pong Grain Buffers**:
   Each grain holds a dedicated 16-bit PCM buffer: `grain_[0].pcm[2048]` (4 KB) and `grain_[1].pcm[2048]` (4 KB). Total: 8 KB.
2. **Interleaved Background Decoding (2 Samples / Tick)**:
   During the 1024 samples while Grain 0 is active, the engine decodes **2 samples per audio tick** into Grain 1's PCM buffer.
   $$\text{CPU Cost per Sample} \approx 2 \times 35\text{ cycles} \approx 70\text{ cycles} \approx 0.35\ \mu\text{s (1.7% of the sample period)}$$
   By tick 1024, Grain 1 is 100% decoded with pristine 16-bit fidelity and begins playing instantly. Grain 0 then begins background preparation for its next cycle.
3. **Forward & Reverse Breakcore Stretch**:
   Because the decoded 16-bit PCM resides in SRAM, the engine can render each grain in both directions with zero extra CPU overhead:
   * **Forward Grain**: reads `pcm[age]`.
   * **Reverse Grain**: reads `pcm[2047 - age]`.
   * **Virtual Head Crawl**:
     * Forward: `source_head += (1.0 / stretch)`.
     * Reverse: `source_head -= (1.0 / stretch)`.
   Reverse stretch is triggered dynamically by **Bit 3 of the Turing Machine shift register** (`t_reg & 0x08`) or when zone intensity exceeds 3400. This produces authentic breakcore reverse rolls, swelling snare hits, and backward time-stretched breaks without dropouts.

### 5.4 Why Flash Memory Cannot Be Used For Live Recording (Even with 16MB Flash)
While RP2040 QSPI Flash read bandwidth is ample ($\approx 20\text{ MB/s}$ via the 16 KB XIP cache), **live real-time audio recording directly into SPI Flash is physically impossible on the RP2040**:
* **Sector Erase Latency**: NOR SPI Flash cannot overwrite existing bytes without first erasing a 4 KB sector. A 4 KB sector erase takes **30 ms to 100 ms**, during which the SPI bus is locked and the XIP cache stalls all CPU execution from flash. At 48 kHz, a 50 ms freeze drops 2400 audio samples, producing catastrophic audio gaps.
* **Flash Endurance**: SPI flash sectors degrade after $\approx 100\,000$ write/erase cycles. Continuous live circular recording into flash would destroy the flash chip within a few hours of live play.
* **SRAM Superiority**: RP2040's 264 KB internal SRAM operates with single-cycle zero-latency read/write access and infinite write endurance. Stuturing stores up to 8 seconds of audio in 192 KB of SRAM using IMA-ADPCM, while retaining 48 KB of free SRAM for the keyframe grid, grain PCM buffers, and peripherals.

---

## 6. Software Architecture & Modularity

The code is strictly separated into focused, reusable modules following object-oriented embedded C++ best practices:

* **`adpcm.h`**: Stateless codec primitive with 1st-order error-feedback noise shaping (`adpcm_encode_shaped`, `adpcm_decode`).
* **`adpcm_buffer.h` (`AdpcmBuffer`)**: Encapsulates the 192 KB SRAM ring buffer, dense 64-sample keyframe grid, and bit-exact state retrieval.
* **`granular_engine.h` (`GranularEngine`)**: Implements dual-grain 2048-sample TD-SOLA synthesis with incremental background preparation, forward/reverse rendering, and cubic stretch scaling.
* **`turing_engine.h` (`TuringEngine`)**: Manages the 16-bit shift register, probabilistic bit inversion, and parameter derivation.
* **`ui_controller.h` (`UIController`)**: Manages switch debouncing (20 ms lock), ADC exponential moving average (EMA) smoothing, and asymmetric deadband hysteresis.
* **`main.cpp` (`Stuturing`)**: Top-level coordinator running at 200 MHz binding audio IO, clock synchronisation, state transitions, and PWM LED diagnostics.
