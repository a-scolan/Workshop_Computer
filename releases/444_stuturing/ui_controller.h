#ifndef UI_CONTROLLER_H
#define UI_CONTROLLER_H

#include "ComputerCard.h"
#include <stdint.h>

/**
 * UIController — Manages hardware debouncing, ADC EMA filtering,
 * and asymmetric deadband hysteresis for Stuturing controls.
 */
class UIController {
public:
    enum RecState {
        REC_IDLE_LIVE,      // Switch MID: continuous sliding buffer
        REC_ARMED_WAIT,     // Switch DOWN pressed: wait for next clock tick
        REC_RECORDING,      // Recording window active, counting clock pulses
        REC_FINISHING,      // Switch DOWN released: finish at next clock tick
        REC_FROZEN          // Switch UP: buffer write locked
    };

    enum SwitchEvent {
        SW_EVENT_NONE = 0,
        SW_EVENT_DOWN_PRESSED,
        SW_EVENT_DOWN_RELEASED,
        SW_EVENT_UP_PRESSED,
        SW_EVENT_UP_RELEASED
    };

    UIController() = default;

    SwitchEvent UpdateSwitch(ComputerCard::Switch raw) {
        SwitchEvent event = SW_EVENT_NONE;
        if (raw != last_raw_switch_) {
            last_raw_switch_ = raw;
            switch_debounce_samples_ = 960; // 20ms at 48kHz
        } else if (switch_debounce_samples_ > 0) {
            --switch_debounce_samples_;
            if (switch_debounce_samples_ == 0) {
                event = apply_switch_change(raw);
            }
        }
        return event;
    }

    void UpdateKnobs(int32_t raw_main, int32_t raw_x, int32_t raw_y, int32_t cv1) {
        // CV1 adds bipolar modulation to Knob X (Turing mutation probability)
        int32_t mod_x = raw_x + cv1;
        if (mod_x < 0) mod_x = 0;
        if (mod_x > 4095) mod_x = 4095;

        // Exponential Moving Average (alpha = 16/256 = 1/16)
        ema_knob_main_ += (raw_main - (ema_knob_main_ >> 8)) * 16;
        ema_knob_x_    += (mod_x - (ema_knob_x_ >> 8)) * 16;
        ema_knob_y_    += (raw_y - (ema_knob_y_ >> 8)) * 16;

        // Main Knob: 8 combo zones of width 512 with +/-24 point deadband hysteresis
        int32_t smooth_main = ema_knob_main_ >> 8;
        if (smooth_main < 0) smooth_main = 0;
        if (smooth_main > 4095) smooth_main = 4095;

        constexpr int32_t ZONE_SIZE = 512;
        constexpr int32_t HYST = 24;

        int current = combo_zone_;
        int32_t lower_bound = current * ZONE_SIZE - HYST;
        int32_t upper_bound = (current + 1) * ZONE_SIZE + HYST;

        if (smooth_main < lower_bound) {
            current = smooth_main / ZONE_SIZE;
            if (current < 0) current = 0;
        } else if (smooth_main >= upper_bound) {
            current = smooth_main / ZONE_SIZE;
            if (current > 7) current = 7;
        }
        combo_zone_ = current;

        // Intensity within zone (0..4095) with saturation plateau at 80% (buffer zone on the last 20%)
        // ZONE_SIZE = 512. 80% of 512 = ~410 samples.
        // rem in [0 .. 409] scales linearly to [0 .. 4095].
        // rem in [410 .. 511] is clamped at 4095, providing a safe deadband before switching combos.
        int32_t rem = smooth_main - (combo_zone_ * ZONE_SIZE);
        if (rem < 0) rem = 0;
        if (rem >= ZONE_SIZE) rem = ZONE_SIZE - 1;

        constexpr int32_t ACTIVE_ZONE_SPAN = (ZONE_SIZE * 4) / 5; // 80% = 409
        if (rem >= ACTIVE_ZONE_SPAN) {
            zone_intensity_ = 4095;
        } else {
            zone_intensity_ = (rem * 4095) / ACTIVE_ZONE_SPAN;
        }

        // Knob Y: Ratchet subdivisions (1, 2, 3, 4, 6, 8, 16)
        int32_t smooth_y = ema_knob_y_ >> 8;
        static constexpr int32_t kRatchets[7] = {1, 2, 3, 4, 6, 8, 16};
        int y_idx = (smooth_y * 7) / 4096;
        if (y_idx > 6) y_idx = 6;
        ratchet_div_ = kRatchets[y_idx];
        ratchet_idx_ = y_idx;

        if (last_display_smooth_y_ < 0) {
            last_display_smooth_y_ = smooth_y;
            prev_ratchet_idx_ = y_idx;
        } else {
            int32_t diff = smooth_y - last_display_smooth_y_;
            if (diff < 0) diff = -diff;
            if (y_idx != prev_ratchet_idx_ || diff > 25) {
                last_display_smooth_y_ = smooth_y;
                prev_ratchet_idx_ = y_idx;
                ratchet_moved_ = true;
            }
        }
    }

    RecState GetRecState() const { return rec_state_; }
    void SetRecState(RecState state) { rec_state_ = state; }

    ComputerCard::Switch GetDebouncedSwitch() const { return debounced_switch_; }
    int GetComboZone() const { return combo_zone_; }
    int32_t GetZoneIntensity() const { return zone_intensity_; }
    int32_t GetRatchetDiv() const { return ratchet_div_; }
    int GetRatchetIndex() const { return ratchet_idx_; }
    int32_t GetTuringProb() const { return ema_knob_x_ >> 8; }

    bool CheckAndClearRatchetMoved() {
        bool m = ratchet_moved_;
        ratchet_moved_ = false;
        return m;
    }

private:
    SwitchEvent apply_switch_change(ComputerCard::Switch sw) {
        ComputerCard::Switch old_sw = debounced_switch_;
        debounced_switch_ = sw;

        SwitchEvent event = SW_EVENT_NONE;
        if (sw == ComputerCard::Switch::Down) {
            event = SW_EVENT_DOWN_PRESSED;
        } else if (old_sw == ComputerCard::Switch::Down && sw != ComputerCard::Switch::Down) {
            event = SW_EVENT_DOWN_RELEASED;
        } else if (sw == ComputerCard::Switch::Up) {
            event = SW_EVENT_UP_PRESSED;
        } else if (old_sw == ComputerCard::Switch::Up && sw == ComputerCard::Switch::Middle) {
            event = SW_EVENT_UP_RELEASED;
        }
        return event;
    }

    RecState rec_state_ = REC_IDLE_LIVE;
    int32_t  switch_debounce_samples_ = 0;
    ComputerCard::Switch debounced_switch_ = ComputerCard::Switch::Middle;
    ComputerCard::Switch last_raw_switch_ = ComputerCard::Switch::Middle;

    int32_t  ema_knob_main_ = 2048 << 8;
    int32_t  ema_knob_x_    = 2048 << 8;
    int32_t  ema_knob_y_    = 0;

    int      combo_zone_ = 0;
    int32_t  zone_intensity_ = 0;
    int32_t  ratchet_div_ = 1;
    int      ratchet_idx_ = 0;
    int      prev_ratchet_idx_ = -1;
    int32_t  last_display_smooth_y_ = -1;
    bool     ratchet_moved_ = false;
};

#endif // UI_CONTROLLER_H
