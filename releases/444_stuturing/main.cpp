// Stuturing — Stuttering on Turing Machine with Slice-Aligned IMA-ADPCM,
// Quantized Window Recording, Hysteresis Combinations, and Granular Time-Stretching.
//
// Target: Workshop Computer (Raspberry Pi RP2040)
//
// Controls:
//   Switch:
//     DOWN (Momentary) : Armed/Record clock-quantized window (1 to 8s)
//     MID              : Live Stutter mode (sliding window recorded and stuttered continuously)
//     UP (Latching)    : Freeze current window and loop/stutter indefinitely
//   Main Knob          : 8 Combo Zones (Clean, S, R, T, S+R, S+T, R+T, All 3)
//                        with EMA filtering + asymmetric deadband hysteresis.
//                        Remainder in zone modulates effect intensity.
//   Knob X             : Turing Machine Mutation Probability (modulated by CV In 2)
//                        ~12h = 50% random mutation (ever-changing)
//                        ~9h / ~3h = slipping loop (infrequent mutations)
//                        ~5h = locked deterministic repeating loop
//                        ~7h = locked double-length loop (inverts every cycle)
//   Knob Y             : Ratchet subdivisions (1, 2, 3, 4, 6, 8, 16 slices per beat)
//
// Jacks:
//   Audio In 1         : Main audio input
//   Audio Out 1        : Processed output (stutured)
//   Audio Out 2        : Always dry pass-through
//   Pulse In 1         : Master clock input (rising edge)
//   Pulse In 2         : External gate (forces stutter when high)
//   Pulse Out 1        : Sub-slice clock pulse
//   Pulse Out 2        : Clock mirror (Pulse In 1)
//   CV In 2            : Modulation added to Knob X (Turing mutation rate)
//   CV Out 1           : Effect gate (+5V during stutter/stretch)
//   CV Out 2           : Slice phase ramp (descending +5V -> 0V)
//
// LEDs (2 columns of 3 LEDs):
//   LED 0 (Top-L)      : Clock pulse (dim) + bright flash at window start/end
//   LED 1 (Mid-L)      : Rec/Freeze status (Pulse = Rec, Solid = Freeze, Off = Live)
//   LED 2 (Bot-L)      : Turing Machine register activity
//   LED 3 (Top-R)      : Shuffle active (bright) / background glow
//   LED 4 (Mid-R)      : Repeat active (bright) / background glow
//   LED 5 (Bot-R)      : Stretch active (bright) / background glow

#include "ComputerCard.h"
#include "adpcm_buffer.h"
#include "granular_engine.h"
#include "turing_engine.h"
#include "ui_controller.h"
#include <stdint.h>

class Stuturing : public ComputerCard {
public:
    Stuturing() : turing_(0x4444u) {}

    void ProcessSample() override {
        // 1. Debounce and switch sampling
        ui_.UpdateSwitch(SwitchVal());

        // 2. Knob filtering (EMA) & Hysteresis
        ui_.UpdateKnobs(KnobVal(Knob::Main), KnobVal(Knob::X), KnobVal(Knob::Y), CVIn2());

        // 3. Clock & Quantized Window Management
        bool clock_tick = PulseIn1RisingEdge();
        bool clock_high = PulseIn1();
        if (clock_tick) {
            handle_clock_pulse();
        }

        // 4. Record incoming audio into ADPCM buffer (unless frozen)
        int16_t in_sample = AudioIn1();
        record_sample(in_sample);

        // 5. Slice Progression & Turing Engine
        advance_slice(clock_tick);

        // 6. Playback & DSP Rendering
        int16_t out_sample = render_audio(in_sample);

        // 7. Output routing
        AudioOut1(out_sample);
        AudioOut2(in_sample); // Always dry pass-through
        PulseOut1(slice_boundary_);
        PulseOut2(clock_high);
        slice_boundary_ = false;

        bool fx_active = (ui_.GetComboZone() > 0) &&
                         (st_shuffle_active_ || st_repeat_active_ || st_stretch_active_ || PulseIn2());
        CVOut1(fx_active ? 2047 : -2048);

        // CV2: slice phase ramp (descending +5V -> 0V)
        if (slice_samples_ > 1) {
            int32_t ramp_cv = 2047 - (int32_t)((int64_t)slice_pos_ * 4095 / (slice_samples_ - 1));
            CVOut2(ramp_cv);
        } else {
            CVOut2(0);
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
    bool    slice_boundary_ = false;

    // Active slice effects
    bool    st_shuffle_active_ = false;
    bool    st_repeat_active_  = false;
    bool    st_stretch_active_ = false;

    // Playback state for non-stretched decoding
    int32_t       read_sample_pos_ = 0;
    adpcm_state_t read_dec_state_ = {0, 0};

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
                }
                break;

            case UIController::REC_FINISHING:
                close_window();
                ui_.SetRecState((ui_.GetDebouncedSwitch() == Switch::Up)
                                ? UIController::REC_FROZEN
                                : UIController::REC_IDLE_LIVE);
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

    void close_window() {
        window_samples_ = buf_.GetWritePos();
        if (window_samples_ < measured_beat_samples_) {
            window_samples_ = measured_beat_samples_;
        }
        if (window_samples_ > AdpcmBuffer::MAX_SAMPLES) {
            window_samples_ = AdpcmBuffer::MAX_SAMPLES;
        }
        window_flash_counter_ = 4800;
        current_slice_idx_ = 0;
        slice_pos_ = 0;
    }

    // -----------------------------------------------------------------------
    // Audio In Recording
    // -----------------------------------------------------------------------
    void record_sample(int16_t sample) {
        ++clock_counter_;

        if (ui_.GetRecState() == UIController::REC_FROZEN) {
            return;
        }

        buf_.RecordSample(sample, slice_samples_);

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
            slice_boundary_ = true;

            // Trigger Turing shift register update
            turing_.Clock(ui_.GetTuringProb());

            evaluate_effects_for_slice();
        }
    }

    void evaluate_effects_for_slice() {
        int combo = ui_.GetComboZone();
        bool want_shuffle = (combo & 1) != 0;
        bool want_repeat  = (combo & 2) != 0;
        bool want_stretch = (combo & 4) != 0;

        uint16_t t_reg = turing_.Register();
        int32_t intensity = ui_.GetZoneIntensity();

        // Repeat evaluation
        st_repeat_active_ = false;
        if (want_repeat) {
            bool t_repeat_bit = (t_reg & 0x01) != 0;
            st_repeat_active_ = t_repeat_bit || ((t_reg & 0xFF) < (intensity >> 4));
        }

        if (!st_repeat_active_) {
            current_slice_idx_ = (current_slice_idx_ + 1) % total_slices_in_window_;
        }

        // Shuffle evaluation
        st_shuffle_active_ = false;
        target_slice_idx_ = current_slice_idx_;
        if (want_shuffle) {
            bool t_shuf_bit = (t_reg & 0x02) != 0;
            if (t_shuf_bit || (((t_reg >> 4) & 0xFF) < (intensity >> 4))) {
                st_shuffle_active_ = true;
                target_slice_idx_ = (turing_.Bits(4, 4)) % total_slices_in_window_;
            }
        }

        // Stretch evaluation (Bit 2 - independent trigger)
        st_stretch_active_ = false;
        AdpcmBuffer::SliceKeyframe kf = buf_.GetKeyframe(target_slice_idx_);
        read_dec_state_ = kf.dec_state;
        read_sample_pos_ = target_slice_idx_ * slice_samples_;

        if (want_stretch) {
            bool t_stretch_bit = (t_reg & 0x04) != 0;
            st_stretch_active_ = t_stretch_bit || (((t_reg >> 4) & 0x3F) < (intensity >> 6));
            if (st_stretch_active_) {
                granular_.SetBaseState(kf.dec_state);
                granular_.TriggerSlice(target_slice_idx_, slice_samples_, intensity, buf_);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Playback & DSP Rendering
    // -----------------------------------------------------------------------
    int16_t render_audio(int16_t dry_in) {
        if (ui_.GetRecState() == UIController::REC_ARMED_WAIT && buf_.GetWritePos() == 0) {
            return dry_in;
        }

        int16_t raw_sample = 0;

        if (st_stretch_active_) {
            raw_sample = granular_.RenderSample(buf_, window_samples_, slice_pos_);
        } else {
            raw_sample = buf_.DecodeAt(read_sample_pos_ + slice_pos_, window_samples_, &read_dec_state_);
        }

        // 64-sample micro-fade at slice boundaries
        return apply_micro_fade(raw_sample, slice_pos_, slice_samples_);
    }

    inline int16_t apply_micro_fade(int16_t s, int32_t pos, int32_t len) const {
        if (len < 128) return s;
        int32_t amp = 64;
        if (pos < 64) {
            amp = pos;
        } else if (pos >= len - 64) {
            amp = (len - pos) - 1;
        }
        return (int16_t)((s * amp) >> 6);
    }

    // -----------------------------------------------------------------------
    // LED Indicators
    // -----------------------------------------------------------------------
    void update_leds(bool clock_high) {
        // LED 0: Clock beat (dim 600) + bright flash (4095) at window boundary
        uint16_t led0 = clock_high ? 600 : 0;
        if (window_flash_counter_ > 0) {
            --window_flash_counter_;
            led0 = 4095;
        }
        LedBrightness(0, led0);

        // LED 1: Status (Rec = Pulsing, Freeze = Solid, Live = Off)
        uint16_t led1 = 0;
        auto rec_state = ui_.GetRecState();
        if (rec_state == UIController::REC_FROZEN) {
            led1 = 3500;
        } else if (rec_state == UIController::REC_RECORDING || rec_state == UIController::REC_ARMED_WAIT) {
            led1 = (clock_counter_ & 0x800) ? 4000 : 800;
        }
        LedBrightness(1, led1);

        // LED 2: Turing Machine register activity
        uint16_t led2 = (turing_.Register() & 0x01) ? 3000 : 300;
        LedBrightness(2, led2);

        // Right Column (LEDs 3, 4, 5) — Effect status & real-time activity:
        // - OFF: Effect not armed in current combo
        // - GLOW (200): Effect armed in combo, waiting / idle on this subslice
        // - BRIGHT (4000): Effect actively firing on this subslice
        constexpr uint16_t GLOW = 200;
        int combo = ui_.GetComboZone();
        bool s_armed = (combo & 1) != 0;
        bool r_armed = (combo & 2) != 0;
        bool t_armed = (combo & 4) != 0;

        uint16_t led3 = !s_armed ? 0 : (st_shuffle_active_ ? 4000 : GLOW); // Shuffle
        uint16_t led4 = !r_armed ? 0 : (st_repeat_active_  ? 4000 : GLOW); // Repeat
        uint16_t led5 = !t_armed ? 0 : (st_stretch_active_ ? 4000 : GLOW); // Stretch

        LedBrightness(3, led3);
        LedBrightness(4, led4);
        LedBrightness(5, led5);
    }
};

int main() {
    Stuturing card;
    card.Run();
    return 0;
}
