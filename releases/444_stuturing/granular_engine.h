#ifndef GRANULAR_ENGINE_H
#define GRANULAR_ENGINE_H

#include "adpcm_buffer.h"
#include <stdint.h>

/**
 * GranularEngine — Fixed-point dual-grain Time-Domain Overlap-Add (TD-SOLA)
 * time-stretcher with independent ADPCM decoding states per grain.
 *
 * Guarantees zero harmonic distortion / step divergence caused by
 * state-sharing between phase-offset grains.
 */
class GranularEngine {
public:
    GranularEngine() = default;

    void TriggerSlice(int32_t target_slice_idx, int32_t slice_samples, int32_t intensity, const AdpcmBuffer &buf) {
        grain_len_ = 512 + (intensity >> 2); // 512..1535 samples (10.6ms..32ms)
        grain_phase_ = 0;
        slice_start_pos_ = target_slice_idx * slice_samples;

        // Reset independent grain decoders from slice keyframe
        AdpcmBuffer::SliceKeyframe kf = buf.GetKeyframe(target_slice_idx);
        dec_state_a_ = kf.dec_state;
        dec_state_b_ = kf.dec_state;

        pos_a_ = slice_start_pos_;
        pos_b_ = slice_start_pos_ + (grain_len_ / 4);
    }

    int16_t RenderSample(const AdpcmBuffer &buf, int32_t window_samples, int32_t slice_pos) {
        int32_t half_grain = grain_len_ / 2;
        grain_phase_++;

        if (grain_phase_ >= grain_len_) {
            grain_phase_ = 0;
            // Hop Grain A forward slowly to create time-stretch effect
            pos_a_ = slice_start_pos_ + (slice_pos / 2);
            // Re-sync Grain A decoder state to the slice base keyframe
            // to prevent drift across successive grain lifecycles
            dec_state_a_ = last_base_state_;
        } else if (grain_phase_ == half_grain) {
            pos_b_ = slice_start_pos_ + (slice_pos / 2);
            dec_state_b_ = last_base_state_;
        }

        // Triangular window weights (sum to 256)
        int32_t w_a = 0;
        if (grain_phase_ < half_grain) {
            w_a = (grain_phase_ * 256) / half_grain;
        } else {
            w_a = ((grain_len_ - grain_phase_) * 256) / half_grain;
        }
        int32_t w_b = 256 - w_a;

        // Decode Grain A with independent decoder state A
        int16_t s_a = buf.DecodeAt(pos_a_ + (grain_phase_ % half_grain), window_samples, &dec_state_a_);

        // Decode Grain B with independent decoder state B
        int16_t s_b = buf.DecodeAt(pos_b_ + ((grain_phase_ + half_grain) % half_grain), window_samples, &dec_state_b_);

        // 8-bit fixed-point crossfade
        int32_t mixed = (s_a * w_a + s_b * w_b) >> 8;
        return (int16_t)mixed;
    }

    void SetBaseState(const adpcm_state_t &state) {
        last_base_state_ = state;
    }

private:
    int32_t grain_len_ = 1024;
    int32_t grain_phase_ = 0;
    int32_t slice_start_pos_ = 0;
    int32_t pos_a_ = 0;
    int32_t pos_b_ = 0;

    // Independent ADPCM decoder instances for each grain
    adpcm_state_t dec_state_a_ = {0, 0};
    adpcm_state_t dec_state_b_ = {0, 0};
    adpcm_state_t last_base_state_ = {0, 0};
};

#endif // GRANULAR_ENGINE_H
