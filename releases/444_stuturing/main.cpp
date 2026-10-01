// Stuturing — Stuttering on Turing Machine with Slice-Aligned IMA-ADPCM,
// Quantized Window Recording, Hysteresis Combinations, and Granular Time-Stretching.
//
// Target: Workshop Computer (Raspberry Pi RP2040)
//
// Controls:
//   Switch:
//     DOWN (Momentary) : Nearest-beat clock-quantized window record (1 to 8s)
//     MID              : Live Stutter mode (sliding window recorded and stuttered continuously)
//     UP (Latching)    : Freeze / Frippertronics Tape Loop (Sound-on-Sound overdub)
//   Main Knob          : 8 Combo Zones:
//                        1. Clean
//                        2. Shuffle (S)
//                        3. Repeat (R)
//                        4. Stretch (T)
//                        5. Shuffle + Repeat (S+R)
//                        6. Shuffle + Stretch (S+T)
//                        7. Repeat + Stretch (R+T)
//                        8. All 3 (S+R+T)
//                        with EMA filtering, asymmetric deadband hysteresis, and an 80%
//                        saturation plateau (the last 20% acts as a safe buffer before zone switch).
//   Knob X             : Turing Machine Mutation Probability (in Live/Record) /
//                        Frippertronics Decay Feedback (in Freeze mode):
//                        ~7h = 85% (~4 passes), ~12h = 92% (~8 passes), ~5h = 99.2% (>80 passes).
//                        Modulated bipolarly by CV In 1.
//   Knob Y             : Ratchet subdivisions (1, 2, 3, 4, 6, 8, 16 slices per beat)
//
// Jacks:
//   Audio In 1         : Main audio input
//   Audio Out 1        : Processed output (stutured)
//   Audio Out 2        : Always dry pass-through
//   Pulse In 1         : Master clock input (rising edge)
//   Pulse In 2         : External gate (forces stutter ratchet when HIGH)
//   Pulse Out 1        : Sub-slice clock pulse
//   Pulse Out 2        : Clock mirror (Pulse In 1)
//   CV In 1            : Bipolar modulation added to Knob X (Turing rate / Frippertronics decay)
//   CV In 2            : Slice Scrubbing / Navigation across the active window
//   CV Out 1           : Effect gate (+5V during stutter/stretch)
//   CV Out 2           : Slice phase ramp (descending +5V -> 0V)
//
// LEDs (2 columns of 3 LEDs):
//   Hardware numbering:
//     0 (Top-L)    1 (Top-R)
//     2 (Mid-L)    3 (Mid-R)
//     4 (Bot-L)    5 (Bot-R)
//
//   Left Column (System & Clock):
//     LED 0 (Top-L) : Clock pulse (dim) + bright flash at window start/end
//     LED 2 (Mid-L) : Rec/Freeze status (Pulse = Rec, Solid = Freeze, Off = Live)
//     LED 4 (Bot-L) : Turing Machine register activity
//
//   Right Column (Effects):
//     LED 1 (Top-R) : Shuffle active (bright) / background glow
//     LED 3 (Mid-R) : Repeat active (bright) / background glow
//     LED 5 (Bot-R) : Stretch active (bright) / background glow

#include "ComputerCard.h"
#include "adpcm_buffer.h"
#include "granular_engine.h"
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "turing_engine.h"
#include "ui_controller.h"
#include <stdint.h>

class Stuturing : public ComputerCard {
public:
    Stuturing() : turing_(0x4444u) {}

    void ProcessSample() override {
        // 1. Debounce and switch sampling
        UIController::SwitchEvent sw_ev = ui_.UpdateSwitch(SwitchVal());
        if (sw_ev != UIController::SW_EVENT_NONE) {
            handle_switch_event(sw_ev);
        }

        // 2. Knob filtering (EMA) & Hysteresis
        // CV In 1: bipolar modulation added to Knob X (Turing mutation or Fripper decay)
        ui_.UpdateKnobs(KnobVal(Knob::Main), KnobVal(Knob::X), KnobVal(Knob::Y), CVIn1());
        if (ui_.CheckAndClearRatchetMoved()) {
            div_display_timer_ = 57600; // ~1.2s at 48kHz
        }

        // 3. Clock & Quantized Window Management
        bool clock_tick = PulseIn1RisingEdge();
        bool clock_high = PulseIn1();
        if (clock_tick) {
            handle_clock_pulse();
        }

        // 4. Record incoming audio into ADPCM buffer (live sliding, quantized record, or Fripper overdub)
        int16_t in_sample = AudioIn1();
        record_sample(in_sample);

        // 5. Slice Progression & Turing Engine
        advance_slice(clock_tick);

        // 6. Playback & DSP Rendering
        int16_t out_sample = render_audio(in_sample);

        // 7. Output routing
        AudioOut1(out_sample);
        AudioOut2(in_sample); // Always dry pass-through
        PulseOut1(pulse_out1_timer_ > 0);
        if (pulse_out1_timer_ > 0) {
            --pulse_out1_timer_;
        }
        PulseOut2(clock_high);

        bool fx_active = st_shuffle_active_ || st_repeat_active_ || st_stretch_active_ || PulseIn2();
        CVOut1(fx_active ? 1700 : -2048);

        // CV2: slice phase ramp (descending +5V -> 0V, uncalibrated DAC: 1700 -> -2048)
        if (slice_samples_ > 1) {
            int32_t ramp_cv = 1700 - (int32_t)((int64_t)slice_pos_ * 3748 / (slice_samples_ - 1));
            CVOut2((int16_t)ramp_cv);
        } else {
            CVOut2(-2048);
        }

        // 8. Visual feedback (LEDs)
        update_leds(clock_high);
    }

private:
    // Modular Subsystems
    AdpcmBuffer    buf_;
    GranularEngine granular_;
    TuringEngine   turing_;
    UIController   ui_;

    // Window & Timing State
    int32_t window_clock_ticks_ = 0;
    int32_t window_samples_ = 48000;        // Default 1s
    int32_t measured_beat_samples_ = 24000; // Default 120 BPM
    int32_t clock_counter_ = 0;
    int32_t window_flash_counter_ = 0;

    // Slice progression
    int32_t slice_samples_ = 48000;
    int32_t slice_pos_ = 0;
    int32_t current_slice_idx_ = 0;
    int32_t target_slice_idx_ = 0;
    int32_t total_slices_in_window_ = 1;
    int32_t pulse_out1_timer_ = 0;

    // Active slice effects
    bool    st_shuffle_active_ = false;
    bool    st_repeat_active_  = false;
    bool    st_stretch_active_ = false;

    // Sticky LED timers
    int32_t shuffle_led_timer_ = 0;
    int32_t repeat_led_timer_  = 0;
    int32_t stretch_led_timer_ = 0;
    int32_t div_display_timer_ = 0;

    // Playback state for non-stretched decoding
    int32_t       read_sample_pos_ = 0;
    adpcm_state_t read_dec_state_ = {0, 0};
    int16_t       last_rendered_sample_ = 0;
    int16_t       last_slice_end_sample_ = 0;

    // -----------------------------------------------------------------------
    // Switch Event Handling (Nearest-Beat Quantization & State Machine)
    // -----------------------------------------------------------------------
    void handle_switch_event(UIController::SwitchEvent ev) {
        auto rec_state = ui_.GetRecState();

        if (ev == UIController::SW_EVENT_DOWN_PRESSED) {
            // When pressing DOWN (Record):
            // Check if we pressed just after the clock beat (within the first 50% of the beat).
            // If so, snap to the beat that just passed!
            int32_t beat_len = measured_beat_samples_;
            if (beat_len < 200) beat_len = 200;

            if (clock_counter_ < (beat_len / 2)) {
                // Retroactively start recording from the previous beat!
                ui_.SetRecState(UIController::REC_RECORDING);
                window_clock_ticks_ = 1;
                buf_.SetWritePos(clock_counter_);
                window_flash_counter_ = 4800; // 100ms flash
            } else {
                // Closer to the upcoming beat: arm and wait for next clock tick
                ui_.SetRecState(UIController::REC_ARMED_WAIT);
            }
        } else if (ev == UIController::SW_EVENT_DOWN_RELEASED) {
            // When releasing DOWN (Stop recording):
            if (rec_state == UIController::REC_RECORDING) {
                int32_t beat_len = measured_beat_samples_;
                if (beat_len < 200) beat_len = 200;

                if (clock_counter_ < (beat_len / 2) && window_clock_ticks_ >= 1) {
                    // Released just after the beat: snap to the beat that just passed!
                    // Truncate the window exactly to the last integer beat boundary
                    int32_t exact_window = (window_clock_ticks_ - 1) * beat_len;
                    if (exact_window < beat_len) {
                        exact_window = beat_len;
                    }
                    close_window_to_length(exact_window);
                    ui_.SetRecState((ui_.GetDebouncedSwitch() == Switch::Up)
                                    ? UIController::REC_FROZEN
                                    : UIController::REC_IDLE_LIVE);
                    if (ui_.GetDebouncedSwitch() == Switch::Up) {
                        buf_.ResetOverdubStates(0);
                    }
                } else {
                    // Released before the midpoint: wait to complete on next clock pulse
                    ui_.SetRecState(UIController::REC_FINISHING);
                }
            } else if (rec_state == UIController::REC_ARMED_WAIT) {
                // Cancelled before ever starting
                ui_.SetRecState(UIController::REC_IDLE_LIVE);
            }
        } else if (ev == UIController::SW_EVENT_UP_PRESSED) {
            ui_.SetRecState(UIController::REC_FROZEN);
            buf_.ResetOverdubStates(0);
        } else if (ev == UIController::SW_EVENT_UP_RELEASED) {
            ui_.SetRecState(UIController::REC_IDLE_LIVE);
        }
    }

    // -----------------------------------------------------------------------
    // Clock Pulses & Window Quantization
    // -----------------------------------------------------------------------
    void handle_clock_pulse() {
        if (clock_counter_ > 200) {
            measured_beat_samples_ = clock_counter_;
        }
        clock_counter_ = 0;

        auto rec_state = ui_.GetRecState();
        switch (rec_state) {
            case UIController::REC_ARMED_WAIT:
                ui_.SetRecState(UIController::REC_RECORDING);
                window_clock_ticks_ = 1;
                buf_.SetWritePos(0);
                buf_.ResetNoiseShaping();
                window_flash_counter_ = 4800; // 100ms flash
                break;

            case UIController::REC_RECORDING:
                ++window_clock_ticks_;
                if (buf_.GetWritePos() + measured_beat_samples_ >= AdpcmBuffer::MAX_SAMPLES) {
                    close_window();
                    ui_.SetRecState(UIController::REC_FROZEN);
                    buf_.ResetOverdubStates(0);
                }
                break;

            case UIController::REC_FINISHING:
                close_window();
                ui_.SetRecState((ui_.GetDebouncedSwitch() == Switch::Up)
                                ? UIController::REC_FROZEN
                                : UIController::REC_IDLE_LIVE);
                if (ui_.GetDebouncedSwitch() == Switch::Up) {
                    buf_.ResetOverdubStates(0);
                }
                break;

            case UIController::REC_IDLE_LIVE:
                window_samples_ = measured_beat_samples_;
                if (window_samples_ > AdpcmBuffer::MAX_SAMPLES) {
                    window_samples_ = AdpcmBuffer::MAX_SAMPLES;
                }
                break;

            case UIController::REC_FROZEN:
            default:
                break;
        }
    }

    void close_window_to_length(int32_t len) {
        window_samples_ = len;
        if (window_samples_ < measured_beat_samples_) {
            window_samples_ = measured_beat_samples_;
        }
        if (window_samples_ > AdpcmBuffer::MAX_SAMPLES) {
            window_samples_ = AdpcmBuffer::MAX_SAMPLES;
        }
        buf_.SetWritePos(0);
        window_flash_counter_ = 4800;
        current_slice_idx_ = 0;
        slice_pos_ = 0;
    }

    void close_window() {
        close_window_to_length(buf_.GetWritePos());
    }

    // -----------------------------------------------------------------------
    // Audio In Recording
    // -----------------------------------------------------------------------
    void record_sample(int16_t sample) {
        ++clock_counter_;

        if (ui_.GetRecState() == UIController::REC_FROZEN) {
            // Frippertronics Overdub: continuous decaying sound-on-sound
            // with 1-pole tape damping LPF, decay feedback and soft-saturation.
            // In Freeze mode, Knob X (+ CV In 1) controls loop persistence/decay length:
            // ~7h (0): 85% feedback (~4 passes decay)
            // ~12h (2048): 92% feedback (~8 passes decay, optimal musical sweet spot)
            // ~5h (4095): 99.2% feedback (~88 passes decay, near-infinite sound-on-sound tape drone)
            int32_t t_prob = ui_.GetTuringProb(); // 0..4095
            int32_t decay_factor = 218 + (t_prob * 36) / 4095; // 218..254
            buf_.OverdubSample(sample, window_samples_, decay_factor);
            return;
        }

        buf_.RecordSample(sample);

        if (ui_.GetRecState() == UIController::REC_IDLE_LIVE) {
            if (buf_.GetWritePos() >= window_samples_) {
                buf_.SetWritePos(0);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Slice Progression & Turing Engine
    // -----------------------------------------------------------------------
    void advance_slice(bool clock_tick) {
        int32_t beat_len = measured_beat_samples_;
        if (beat_len < 100) beat_len = 100;

        slice_samples_ = beat_len / ui_.GetRatchetDiv();
        if (slice_samples_ < 64) slice_samples_ = 64;

        total_slices_in_window_ = window_samples_ / slice_samples_;
        if (total_slices_in_window_ < 1) total_slices_in_window_ = 1;

        slice_pos_++;
        if (clock_tick || slice_pos_ >= slice_samples_) {
            slice_pos_ = 0;
            last_slice_end_sample_ = last_rendered_sample_;
            // 2 ms trigger pulse at 48 kHz (96 samples, capped to avoid overlap if slices are tiny)
            int32_t trig_len = 96;
            if (trig_len > slice_samples_ / 2) {
                trig_len = slice_samples_ / 2;
                if (trig_len < 1) trig_len = 1;
            }
            pulse_out1_timer_ = trig_len;

            // In Freeze mode, Turing shift register does NOT mutate (locked deterministic loop)
            // so the Turing phrase remains deterministic and locked while adjusting Frippertronics decay!
            int32_t t_prob = (ui_.GetRecState() == UIController::REC_FROZEN) ? 4095 : ui_.GetTuringProb();
            turing_.Clock(t_prob);

            evaluate_effects_for_slice();
        }
    }

    void evaluate_effects_for_slice() {
        int combo = ui_.GetComboZone();
        // Combo mapping matching README.md:
        // 0: Clean
        // 1: Shuffle
        // 2: Repeat
        // 3: Stretch
        // 4: Shuffle + Repeat
        // 5: Shuffle + Stretch
        // 6: Repeat + Stretch
        // 7: All 3 (Shuffle + Repeat + Stretch)
        static constexpr uint8_t kComboFlags[8] = {
            0,                      // 0: Clean
            1,                      // 1: Shuffle
            2,                      // 2: Repeat
            4,                      // 3: Stretch
            1 | 2,                  // 4: Shuffle + Repeat
            1 | 4,                  // 5: Shuffle + Stretch
            2 | 4,                  // 6: Repeat + Stretch
            1 | 2 | 4               // 7: All 3
        };
        uint8_t flags = (combo >= 0 && combo <= 7) ? kComboFlags[combo] : 0;
        bool want_shuffle = (flags & 1) != 0;
        bool want_repeat  = (flags & 2) != 0;
        bool want_stretch = (flags & 4) != 0;

        uint16_t t_reg = turing_.Register();
        int32_t intensity = ui_.GetZoneIntensity();

        // Repeat evaluation
        st_repeat_active_ = false;
        if (want_repeat || PulseIn2()) {
            bool t_repeat_bit = (t_reg & 0x01) != 0;
            st_repeat_active_ = PulseIn2() || t_repeat_bit || ((t_reg & 0xFF) < (intensity >> 4));
        }

        if (st_repeat_active_) {
            repeat_led_timer_ = 2160;
        } else {
            current_slice_idx_ = (current_slice_idx_ + 1) % total_slices_in_window_;
        }

        // Shuffle evaluation (or CV In 2 scrubbing across slices)
        st_shuffle_active_ = false;
        target_slice_idx_ = current_slice_idx_;

        // CV In 2: Slice Scrubbing across the active window (-2048..2047 -> 0..total_slices-1)
        if (Connected(Input::CV2) && total_slices_in_window_ > 1) {
            int32_t cv2 = CVIn2();
            int32_t scrub = (cv2 + 2048) * total_slices_in_window_ / 4096;
            if (scrub < 0) scrub = 0;
            if (scrub >= total_slices_in_window_) scrub = total_slices_in_window_ - 1;
            target_slice_idx_ = scrub;
            st_shuffle_active_ = true;
        } else if (want_shuffle) {
            bool t_shuf_bit = (t_reg & 0x02) != 0;
            if (t_shuf_bit || (((t_reg >> 4) & 0xFF) < (intensity >> 4))) {
                st_shuffle_active_ = true;
                target_slice_idx_ = (turing_.Bits(4, 4)) % total_slices_in_window_;
            }
        }

        if (st_shuffle_active_) {
            shuffle_led_timer_ = 2160;
        }

        // Stretch evaluation
        st_stretch_active_ = false;
        read_sample_pos_ = target_slice_idx_ * slice_samples_;
        read_dec_state_ = buf_.GetStateAt(read_sample_pos_);

        if (want_stretch) {
            bool t_stretch_bit = (t_reg & 0x04) != 0;
            bool trigger_stretch = (combo == 3) || t_stretch_bit || (((t_reg >> 4) & 0x3F) < (intensity >> 6));
            if (trigger_stretch) {
                st_stretch_active_ = true;
                stretch_led_timer_ = 2160;
                bool t_reverse_bit = (t_reg & 0x08) != 0;
                bool is_reverse = t_reverse_bit || (intensity > 3400);

                int32_t l_start = 0;
                int32_t l_len = window_samples_;
                if (want_shuffle || want_repeat) {
                    l_start = target_slice_idx_ * slice_samples_;
                    l_len = slice_samples_;
                }
                granular_.SetParameters(l_start, l_len, intensity, is_reverse, buf_);
            }
        }
        if (!st_stretch_active_) {
            granular_.Reset();
        }
    }

    // -----------------------------------------------------------------------
    // Playback & DSP Rendering
    // -----------------------------------------------------------------------
    int16_t render_audio(int16_t dry_in) {
        // If armed and waiting for first recording pass, pass through dry audio
        if (ui_.GetRecState() == UIController::REC_ARMED_WAIT && buf_.GetWritePos() == 0) {
            last_rendered_sample_ = dry_in;
            return dry_in;
        }

        if (st_stretch_active_) {
            int16_t out_sample = granular_.RenderSample(buf_);
            last_rendered_sample_ = out_sample;
            return out_sample;
        }

        int16_t raw_sample = buf_.DecodeAt(read_sample_pos_ + slice_pos_, window_samples_, &read_dec_state_);

        // 16-sample anti-click smooth crossfade at slice boundaries (~0.33ms at 48kHz)
        int16_t out_sample = apply_crossfade(raw_sample, slice_pos_, slice_samples_);
        last_rendered_sample_ = out_sample;
        return out_sample;
    }

    inline int16_t apply_crossfade(int16_t raw, int32_t pos, int32_t len) const {
        constexpr int32_t FADE = 16;
        if (len < FADE * 2) return raw;
        if (pos < FADE) {
            int32_t s = ((int32_t)last_slice_end_sample_ * (FADE - pos) + (int32_t)raw * pos) / FADE;
            return (int16_t)s;
        }
        return raw;
    }

    // -----------------------------------------------------------------------
    // LED Indicators
    // -----------------------------------------------------------------------
    // ComputerCard hardware LED numbering:
    //   0 (Top-L)    1 (Top-R)
    //   2 (Mid-L)    3 (Mid-R)
    //   4 (Bot-L)    5 (Bot-R)
    void update_leds(bool clock_high) {
        // 1. Temporary Division Gauge Display (when Knob Y is moved)
        if (div_display_timer_ > 0) {
            --div_display_timer_;
            int idx = ui_.GetRatchetIndex(); // 0..6
            display_ratchet_gauge(idx);
            return;
        }

        // 2. Normal operational LED display
        // Left Column:
        // LED 0 (Top-L): Clock beat (dim 1200) + bright flash (4095) at window boundary
        uint16_t led0 = clock_high ? 1200 : 0;
        if (window_flash_counter_ > 0) {
            --window_flash_counter_;
            led0 = 4095;
        }
        LedBrightness(0, led0);

        // LED 2 (Mid-L): Status (Rec = Pulsing, Freeze = Solid, Live = Off)
        uint16_t led2 = 0;
        auto rec_state = ui_.GetRecState();
        if (rec_state == UIController::REC_FROZEN) {
            led2 = 3500;
        } else if (rec_state == UIController::REC_RECORDING || rec_state == UIController::REC_ARMED_WAIT) {
            led2 = (clock_counter_ & 0x800) ? 4000 : 1200;
        }
        LedBrightness(2, led2);

        // LED 4 (Bot-L): Turing Machine register activity
        uint16_t led4 = (turing_.Register() & 0x01) ? 3500 : 600;
        LedBrightness(4, led4);

        // Right Column (LEDs 1, 3, 5) — Effect status & real-time activity:
        constexpr uint16_t GLOW = 800;
        int combo = ui_.GetComboZone();
        static constexpr uint8_t kComboFlags[8] = {
            0, 1, 2, 4, 1 | 2, 1 | 4, 2 | 4, 1 | 2 | 4
        };
        uint8_t flags = (combo >= 0 && combo <= 7) ? kComboFlags[combo] : 0;
        bool s_armed = (flags & 1) != 0;
        bool r_armed = (flags & 2) != 0;
        bool t_armed = (flags & 4) != 0;

        if (shuffle_led_timer_ > 0) --shuffle_led_timer_;
        if (repeat_led_timer_ > 0)  --repeat_led_timer_;
        if (stretch_led_timer_ > 0) --stretch_led_timer_;

        uint16_t led1 = !s_armed ? 0 : (shuffle_led_timer_ > 0 ? 4000 : GLOW); // Shuffle (Top-R)
        uint16_t led3 = !r_armed ? 0 : (repeat_led_timer_ > 0  ? 4000 : GLOW); // Repeat  (Mid-R)
        uint16_t led5 = !t_armed ? 0 : (stretch_led_timer_ > 0 ? 4000 : GLOW); // Stretch (Bot-R)

        LedBrightness(1, led1);
        LedBrightness(3, led3);
        LedBrightness(5, led5);
    }

    void display_ratchet_gauge(int idx) {
        static constexpr uint8_t gauge_leds[6] = {4, 2, 0, 5, 3, 1};

        if (idx == 6) { // div 16: rapid blink ~8Hz
            bool blink = (clock_counter_ & 0x800) != 0;
            uint16_t b = blink ? 4000 : 0;
            for (int i = 0; i < 6; ++i) {
                LedBrightness(gauge_leds[i], b);
            }
        } else {
            int count = idx + 1; // 1 to 6
            for (int i = 0; i < 6; ++i) {
                uint8_t led = gauge_leds[i];
                LedBrightness(led, (i < count) ? 4000 : 0);
            }
        }
    }
};

int main() {
    set_sys_clock_khz(200000, true);
    static Stuturing card;
    card.EnableNormalisationProbe();
    card.Run();
    return 0;
}
