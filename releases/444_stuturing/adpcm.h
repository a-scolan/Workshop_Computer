/**
 * adpcm.h — IMA-ADPCM codec (header-only).
 * 4-bit per sample, 4:1 compression of 16-bit audio.
 */

#ifndef ADPCM_H
#define ADPCM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t predictor;
    int8_t  step_index;
} adpcm_state_t;

static const int16_t ima_step_table[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,
    337,371,408,449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,
    1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,
    6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,
    22385,24623,27086,29794,32767
};

static const int8_t ima_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

static inline int adpcm_clamp_index(int idx) {
    if (idx < 0)  return 0;
    if (idx > 88) return 88;
    return idx;
}

static inline int16_t adpcm_clamp16(int32_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/**
 * Encode one 16-bit sample -> 4-bit nybble with 1st-order noise shaping.
 * 
 * Takes input sample and pointer to residual quantization error accumulator (noise_error).
 * Feeds back the previous sample's quantization error into the current input before encoding,
 * shifting the quantization noise spectral energy up towards the Nyquist frequency (24 kHz)
 * where human ear sensitivity is lowest.
 * 
 * Updates codec state and error accumulator.
 */
static inline uint8_t adpcm_encode_shaped(int16_t sample, adpcm_state_t *s, int16_t *noise_error)
{
    // Apply previous quantization error with negative feedback
    int32_t shaped_sample = (int32_t)sample + *noise_error;
    shaped_sample = adpcm_clamp16(shaped_sample);

    int step = ima_step_table[s->step_index];
    int diff = shaped_sample - s->predictor;
    uint8_t nybble = 0;

    if (diff < 0) {
        nybble = 8;
        diff = -diff;
    }

    if (diff >= step)     { nybble |= 4; diff -= step; }
    if (diff >= step / 2) { nybble |= 2; diff -= step / 2; }
    if (diff >= step / 4) { nybble |= 1; }

    int32_t pred = s->predictor;
    int delta = step >> 3;
    if (nybble & 4) delta += step;
    if (nybble & 2) delta += step >> 1;
    if (nybble & 1) delta += step >> 2;
    pred += (nybble & 8) ? -delta : delta;

    s->predictor  = adpcm_clamp16(pred);
    s->step_index = (int8_t)adpcm_clamp_index(s->step_index + ima_index_table[nybble]);

    // Quantization error for next step = actual shaped input - reconstructed predictor
    // Dampened by 7/8 (>>3 subtraction) to guarantee bounded filter stability
    int32_t err = shaped_sample - s->predictor;
    *noise_error = (int16_t)(err - (err >> 3));

    return nybble & 0x0F;
}

/** Encode one 16-bit sample -> 4-bit nybble. Updates state. */
static inline uint8_t adpcm_encode(int16_t sample, adpcm_state_t *s)
{
    int step = ima_step_table[s->step_index];
    int diff = sample - s->predictor;
    uint8_t nybble = 0;

    if (diff < 0) {
        nybble = 8;
        diff = -diff;
    }

    if (diff >= step)     { nybble |= 4; diff -= step; }
    if (diff >= step / 2) { nybble |= 2; diff -= step / 2; }
    if (diff >= step / 4) { nybble |= 1; }

    int32_t pred = s->predictor;
    int delta = step >> 3;
    if (nybble & 4) delta += step;
    if (nybble & 2) delta += step >> 1;
    if (nybble & 1) delta += step >> 2;
    pred += (nybble & 8) ? -delta : delta;

    s->predictor  = adpcm_clamp16(pred);
    s->step_index = (int8_t)adpcm_clamp_index(s->step_index + ima_index_table[nybble]);

    return nybble & 0x0F;
}

/** Decode one 4-bit nybble -> 16-bit sample. Updates state. */
static inline int16_t adpcm_decode(uint8_t nybble, adpcm_state_t *s)
{
    int step = ima_step_table[s->step_index];
    int32_t pred = s->predictor;

    int delta = step >> 3;
    if (nybble & 4) delta += step;
    if (nybble & 2) delta += step >> 1;
    if (nybble & 1) delta += step >> 2;
    pred += (nybble & 8) ? -delta : delta;

    s->predictor  = adpcm_clamp16(pred);
    s->step_index = (int8_t)adpcm_clamp_index(s->step_index + ima_index_table[nybble & 0x0F]);

    return s->predictor;
}

#ifdef __cplusplus
}
#endif

#endif // ADPCM_H
