#ifndef ADPCM_BUFFER_H
#define ADPCM_BUFFER_H

#include "adpcm.h"
#include <stdint.h>

/**
 * AdpcmBuffer — Encapsulates the 192KB SRAM ring buffer, slice keyframes,
 * 1st-order noise shaping encoding, and independent dual-state sample decoding.
 */
class AdpcmBuffer {
public:
    static constexpr int32_t MAX_SAMPLES = 384000;          // 8s at 48kHz
    static constexpr int32_t BUFFER_BYTES = MAX_SAMPLES / 2; // 192,000 bytes
    static constexpr int32_t MAX_KEYFRAMES = 256;
    static constexpr int32_t KEYFRAME_STEP = 64;
    static constexpr int32_t NUM_GRID_KEYFRAMES = MAX_SAMPLES / KEYFRAME_STEP; // 6000 keyframes = 24 KB

    struct SliceKeyframe {
        int32_t sample_offset;
        adpcm_state_t enc_state;
        adpcm_state_t dec_state;
    };

    AdpcmBuffer() {
        Reset();
    }

    void Reset() {
        write_pos_ = 0;
        write_enc_state_ = {0, 0};
        write_noise_error_ = 0;
        for (int i = 0; i < MAX_KEYFRAMES; ++i) {
            keyframes_[i].sample_offset = 0;
            keyframes_[i].enc_state = {0, 0};
            keyframes_[i].dec_state = {0, 0};
        }
        for (int i = 0; i < NUM_GRID_KEYFRAMES; ++i) {
            grid_keyframes_[i] = {0, 0};
        }
    }

    void ResetNoiseShaping() {
        write_noise_error_ = 0;
    }

    void RecordSample(int16_t sample) {
        int32_t pos = write_pos_;
        int32_t byte_idx = pos >> 1;
        bool high_nybble = (pos & 1) != 0;

        // Save dense grid keyframe every 64 samples
        if ((pos & (KEYFRAME_STEP - 1)) == 0) {
            int32_t grid_idx = pos / KEYFRAME_STEP;
            if (grid_idx < NUM_GRID_KEYFRAMES) {
                grid_keyframes_[grid_idx] = write_enc_state_;
            }
        }

        uint8_t nybble = adpcm_encode_shaped(sample, &write_enc_state_, &write_noise_error_);

        if (high_nybble) {
            buf_[byte_idx] = (buf_[byte_idx] & 0x0F) | (nybble << 4);
        } else {
            buf_[byte_idx] = (buf_[byte_idx] & 0xF0) | (nybble & 0x0F);
        }

        write_pos_++;
        if (write_pos_ >= MAX_SAMPLES) {
            write_pos_ = 0;
        }
    }

    // Direct sample decoder with explicit caller-provided state
    inline int16_t DecodeAt(int32_t pos, int32_t window_samples, adpcm_state_t *state) const {
        if (pos < 0) pos = 0;
        if (window_samples > 0 && pos >= window_samples) {
            pos %= window_samples;
        }

        int32_t byte_idx = pos >> 1;
        bool high_nybble = (pos & 1) != 0;

        uint8_t byte = buf_[byte_idx];
        uint8_t nybble = high_nybble ? (byte >> 4) : (byte & 0x0F);

        return adpcm_decode(nybble, state);
    }

    inline uint8_t GetNybbleAt(int32_t pos) const {
        if (pos < 0) pos = 0;
        if (pos >= MAX_SAMPLES) pos %= MAX_SAMPLES;
        int32_t byte_idx = pos >> 1;
        uint8_t byte = buf_[byte_idx];
        return (pos & 1) ? (byte >> 4) : (byte & 0x0F);
    }

    inline void WriteNybbleAt(int32_t pos, uint8_t nybble) {
        if (pos < 0) pos = 0;
        if (pos >= MAX_SAMPLES) pos %= MAX_SAMPLES;
        int32_t byte_idx = pos >> 1;
        if (pos & 1) {
            buf_[byte_idx] = (buf_[byte_idx] & 0x0F) | (nybble << 4);
        } else {
            buf_[byte_idx] = (buf_[byte_idx] & 0xF0) | (nybble & 0x0F);
        }
    }

    void OverdubSample(int16_t dry_sample, int32_t window_samples, int32_t decay_factor = 236) {
        if (window_samples <= 0) return;
        int32_t pos = write_pos_;
        if (pos >= window_samples) pos = 0;

        // When looping around to start of loop (pos == 0),
        // sync overdub states with the keyframe at pos 0 to maintain perfect loop continuity.
        if (pos == 0) {
            overdub_dec_state_ = grid_keyframes_[0];
            overdub_enc_state_ = overdub_dec_state_;
            overdub_noise_error_ = 0;
            overdub_tape_lpf_ = 0;
        }

        // Decode existing recorded sample at this position
        int16_t old_sample = DecodeAt(pos, window_samples, &overdub_dec_state_);

        // Tape damping: 1-pole low-pass filter (cut harsh HF buildup)
        overdub_tape_lpf_ = (overdub_tape_lpf_ + (old_sample * 7)) >> 3;

        // Frippertronics feedback: decay_factor / 256 (e.g. 218..254 -> 85% to 99.2%)
        int32_t fb = (overdub_tape_lpf_ * decay_factor) >> 8;

        // Clean dry audio input scaled to 50% (128/256)
        int32_t dry = dry_sample >> 1;

        int32_t mixed = fb + dry;
        // Soft knee saturation to prevent harsh digital clipping
        if (mixed > 20000) mixed = 20000 + (mixed - 20000) / 2;
        if (mixed > 28000) mixed = 28000;
        if (mixed < -20000) mixed = -20000 + (mixed + 20000) / 2;
        if (mixed < -28000) mixed = -28000;

        // Save grid keyframe every 64 samples BEFORE encoding
        if ((pos & (KEYFRAME_STEP - 1)) == 0) {
            int32_t grid_idx = pos / KEYFRAME_STEP;
            if (grid_idx < NUM_GRID_KEYFRAMES) {
                grid_keyframes_[grid_idx] = overdub_enc_state_;
            }
        }

        uint8_t nybble = adpcm_encode_shaped((int16_t)mixed, &overdub_enc_state_, &overdub_noise_error_);
        WriteNybbleAt(pos, nybble);

        pos++;
        if (pos >= window_samples) {
            pos = 0;
        }
        write_pos_ = pos;
    }

    void ResetOverdubStates(int32_t start_pos = 0) {
        overdub_dec_state_ = GetStateAt(start_pos);
        overdub_enc_state_ = overdub_dec_state_;
        overdub_noise_error_ = 0;
        overdub_tape_lpf_ = 0;
    }

    adpcm_state_t GetStateAt(int32_t pos) const {
        if (pos < 0) pos = 0;
        if (pos >= MAX_SAMPLES) pos %= MAX_SAMPLES;
        int32_t grid_idx = pos >> 6; // pos / KEYFRAME_STEP (64)
        if (grid_idx >= NUM_GRID_KEYFRAMES) grid_idx = NUM_GRID_KEYFRAMES - 1;
        int32_t kf_pos = grid_idx << 6;
        adpcm_state_t state = grid_keyframes_[grid_idx];
        
        int32_t p = kf_pos;
        int32_t byte_idx = p >> 1;
        while (p + 1 < pos) {
            uint8_t b = buf_[byte_idx++];
            adpcm_decode(b & 0x0F, &state);
            adpcm_decode(b >> 4, &state);
            p += 2;
        }
        if (p < pos) {
            uint8_t b = buf_[byte_idx];
            adpcm_decode(b & 0x0F, &state);
        }
        return state;
    }

    SliceKeyframe GetKeyframe(int slice_idx) const {
        return keyframes_[slice_idx % MAX_KEYFRAMES];
    }

    int32_t GetWritePos() const { return write_pos_; }
    void SetWritePos(int32_t pos) { write_pos_ = pos; }

private:
    uint8_t buf_[BUFFER_BYTES];
    SliceKeyframe keyframes_[MAX_KEYFRAMES];
    adpcm_state_t grid_keyframes_[NUM_GRID_KEYFRAMES];

    int32_t write_pos_ = 0;
    adpcm_state_t write_enc_state_ = {0, 0};
    int16_t write_noise_error_ = 0;

    // Overdub engine states
    adpcm_state_t overdub_dec_state_ = {0, 0};
    adpcm_state_t overdub_enc_state_ = {0, 0};
    int16_t overdub_noise_error_ = 0;
    int32_t overdub_tape_lpf_ = 0;
};

#endif // ADPCM_BUFFER_H
