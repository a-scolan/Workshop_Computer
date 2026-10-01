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
    static constexpr int32_t GRAIN_LEN = 2048; // ~42.6ms at 48kHz (Akai / stretchcore sweet spot)
    static constexpr int32_t GRAIN_HOP = 1024; // 50% overlap

    struct Grain {
        int32_t sample_pos = 0;
        int32_t age = 0;
        adpcm_state_t state = {0, 0};
        bool active = false;
    };

    GranularEngine() {
        Reset();
    }

    void Reset() {
        grains_[0] = {0, 0, {0, 0}, false};
        grains_[1] = {0, GRAIN_HOP, {0, 0}, false};
        source_head_q16_ = 0;
        is_initialized_ = false;
        loop_start_ = 0;
        loop_end_ = 48000;
    }

    void SetParameters(int32_t loop_start, int32_t loop_len, int32_t intensity, bool is_reverse, const AdpcmBuffer &buf) {
        if (loop_len < 256) loop_len = 256;
        loop_start_ = loop_start;
        loop_end_ = loop_start + loop_len;
        if (loop_end_ > AdpcmBuffer::MAX_SAMPLES) loop_end_ = AdpcmBuffer::MAX_SAMPLES;
        if (loop_start_ >= loop_end_) loop_start_ = 0;
        is_reverse_ = is_reverse;

        // Fast 32-bit cubic response for stretch factor (1.0x to 8.0x)
        int32_t val = (intensity < 0) ? 0 : (intensity > 4095 ? 4095 : intensity);
        int32_t v = val >> 4; // 0..255
        int32_t eased = (v * v * v) >> 12; // 0..4095
        // stretch_q16: 65536 (1.0x) to 524288 (8.0x)
        int32_t stretch_q16 = 65536 + (eased * 458752) / 4095;
        speed_q16_ = (int32_t)(((uint32_t)65536 << 16) / (uint32_t)stretch_q16);

        // If not initialized yet, initialize both grains smoothly
        if (!is_initialized_) {
            source_head_q16_ = ((int64_t)loop_start_) << 16;

            grains_[0].sample_pos = loop_start_;
            grains_[0].state = buf.GetStateAt(loop_start_);
            grains_[0].age = 0;
            grains_[0].active = true;

            int32_t pos1 = loop_start_ + GRAIN_HOP;
            if (pos1 >= loop_end_) pos1 = loop_start_;
            grains_[1].sample_pos = pos1;
            grains_[1].state = buf.GetStateAt(pos1);
            grains_[1].age = GRAIN_HOP;
            grains_[1].active = true;

            is_initialized_ = true;
        } else {
            // Keep head within new bounds
            int64_t min_head = ((int64_t)loop_start_) << 16;
            int64_t max_head = ((int64_t)loop_end_) << 16;
            if (source_head_q16_ < min_head || source_head_q16_ >= max_head) {
                source_head_q16_ = is_reverse_ ? (max_head - 65536) : min_head;
            }
        }
    }

    int16_t RenderSample(const AdpcmBuffer &buf) {
        if (!is_initialized_) {
            return 0;
        }

        int32_t loop_span = loop_end_ - loop_start_;
        if (loop_span < 256) loop_span = 256;
        int64_t min_head = ((int64_t)loop_start_) << 16;
        int64_t max_head = ((int64_t)loop_end_) << 16;

        // 1. Advance virtual playhead
        if (is_reverse_) {
            source_head_q16_ -= speed_q16_;
            if (source_head_q16_ < min_head || source_head_q16_ >= max_head) {
                source_head_q16_ = max_head - 65536;
            }
        } else {
            source_head_q16_ += speed_q16_;
            if (source_head_q16_ >= max_head || source_head_q16_ < min_head) {
                source_head_q16_ = min_head;
            }
        }

        // 2. Respawn Grain 0 when its life cycle finishes (at age 2048, window weight is ZERO)
        if (grains_[0].age >= GRAIN_LEN) {
            int32_t spawn = (int32_t)(source_head_q16_ >> 16);
            if (spawn < loop_start_) spawn = loop_start_;
            if (spawn >= loop_end_) spawn = loop_start_;
            grains_[0].sample_pos = spawn;
            grains_[0].state = buf.GetStateAt(spawn);
            grains_[0].age = 0;
        }

        // 3. Respawn Grain 1 when its life cycle finishes (at age 2048, window weight is ZERO)
        if (grains_[1].age >= GRAIN_LEN) {
            int32_t spawn = (int32_t)(source_head_q16_ >> 16);
            if (spawn < loop_start_) spawn = loop_start_;
            if (spawn >= loop_end_) spawn = loop_start_;
            grains_[1].sample_pos = spawn;
            grains_[1].state = buf.GetStateAt(spawn);
            grains_[1].age = 0;
        }

        // 4. Decode Grain 0 sample
        int32_t p0 = grains_[0].sample_pos;
        if (p0 >= loop_end_ || p0 < loop_start_) {
            p0 = loop_start_;
            grains_[0].sample_pos = loop_start_;
        }
        uint8_t nybble0 = buf.GetNybbleAt(p0);
        int16_t s0 = adpcm_decode(nybble0, &grains_[0].state);
        grains_[0].sample_pos++;

        // 5. Decode Grain 1 sample
        int32_t p1 = grains_[1].sample_pos;
        if (p1 >= loop_end_ || p1 < loop_start_) {
            p1 = loop_start_;
            grains_[1].sample_pos = loop_start_;
        }
        uint8_t nybble1 = buf.GetNybbleAt(p1);
        int16_t s1 = adpcm_decode(nybble1, &grains_[1].state);
        grains_[1].sample_pos++;

        // 6. Calculate complementary triangular window weights
        uint32_t w0 = (grains_[0].age <= GRAIN_HOP) ? (uint32_t)grains_[0].age : (uint32_t)(GRAIN_LEN - grains_[0].age);
        uint32_t w1 = (grains_[1].age <= GRAIN_HOP) ? (uint32_t)grains_[1].age : (uint32_t)(GRAIN_LEN - grains_[1].age);

        grains_[0].age++;
        grains_[1].age++;

        // 7. Linear crossfade
        int32_t mixed = ((int32_t)s0 * (int32_t)w0 + (int32_t)s1 * (int32_t)w1) >> 10;
        if (mixed < -2048) mixed = -2048;
        if (mixed > 2047) mixed = 2047;
        return (int16_t)mixed;
    }

private:
    Grain   grains_[2];
    int64_t source_head_q16_ = 0;
    int32_t speed_q16_ = 65536;
    int32_t loop_start_ = 0;
    int32_t loop_end_ = 48000;
    bool    is_reverse_ = false;
    bool    is_initialized_ = false;
};

#endif // GRANULAR_ENGINE_H
