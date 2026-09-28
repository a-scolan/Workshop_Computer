#ifndef TURING_ENGINE_H
#define TURING_ENGINE_H

#include <stdint.h>

/**
 * TuringEngine — 16-bit shift register based on the Music Thing Turing Machine.
 *
 * Controls deterministic repetition vs pseudo-random mutation:
 * - Around 12h (~2048): 50% inversion probability (continuous random generation)
 * - Towards 5h (~4095): 0% inversion (locked deterministic loop)
 * - Towards 7h (~0): 100% inversion (alternating double-length loop)
 */
class TuringEngine {
public:
    explicit TuringEngine(uint32_t seed = 0xACE1u) : seed_(seed ? seed : 0xACE1u) {}

    void Clock(int32_t prob_knob) {
        uint32_t rnd = (LCG() >> 20) & 0xFFFu; // 0..4095
        uint16_t msb = (register_ >> (length_ - 1)) & 1u;

        bool invert = false;
        if (prob_knob >= 2048) {
            int32_t invert_chance = (4095 - prob_knob); // 2047 -> 0
            invert = ((int32_t)rnd < invert_chance);
        } else {
            int32_t invert_chance = 2048 + (2048 - prob_knob); // 2048 -> 4096
            invert = ((int32_t)rnd < invert_chance);
        }

        uint16_t mask = (1u << length_) - 1u;
        register_ = ((register_ << 1) | (invert ? (1u - msb) : msb)) & mask;
        step_ = (step_ + 1) % length_;
    }

    uint16_t Register() const { return register_; }

    uint8_t Bits(int start, int count) const {
        return (register_ >> start) & ((1u << count) - 1u);
    }

    int Step() const { return step_; }

private:
    uint32_t seed_;
    uint16_t register_ = 0x5A5Au;
    int length_ = 16;
    int step_ = 0;

    inline uint32_t LCG() {
        seed_ = 1664525u * seed_ + 1013904223u;
        return seed_;
    }
};

#endif // TURING_ENGINE_H
