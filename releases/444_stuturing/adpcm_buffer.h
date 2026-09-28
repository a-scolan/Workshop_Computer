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
    }

    void ResetNoiseShaping() {
        write_noise_error_ = 0;
    }

    void RecordSample(int16_t sample, int32_t slice_samples) {
        int32_t pos = write_pos_;
        int32_t byte_idx = pos >> 1;
        bool high_nybble = (pos & 1) != 0;

        uint8_t nybble = adpcm_encode_shaped(sample, &write_enc_state_, &write_noise_error_);

        if (high_nybble) {
            buf_[byte_idx] = (buf_[byte_idx] & 0x0F) | (nybble << 4);
        } else {
            buf_[byte_idx] = (buf_[byte_idx] & 0xF0) | (nybble & 0x0F);
        }

        // Save slice keyframe at slice boundaries
        if (slice_samples > 0 && (pos % slice_samples) == 0) {
            int kf_idx = (pos / slice_samples) % MAX_KEYFRAMES;
            keyframes_[kf_idx].sample_offset = pos;
            keyframes_[kf_idx].enc_state = write_enc_state_;
            keyframes_[kf_idx].dec_state = write_enc_state_;
            write_noise_error_ = 0;
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

    SliceKeyframe GetKeyframe(int slice_idx) const {
        return keyframes_[slice_idx % MAX_KEYFRAMES];
    }

    int32_t GetWritePos() const { return write_pos_; }
    void SetWritePos(int32_t pos) { write_pos_ = pos; }

private:
    uint8_t buf_[BUFFER_BYTES];
    SliceKeyframe keyframes_[MAX_KEYFRAMES];

    int32_t write_pos_ = 0;
    adpcm_state_t write_enc_state_ = {0, 0};
    int16_t write_noise_error_ = 0;
};

#endif // ADPCM_BUFFER_H
