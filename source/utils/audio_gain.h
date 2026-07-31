/**
 * @file  audio_gain.h
 * @brief Master output gain, shared by the two places that apply it.
 *
 * WHY THIS HEADER EXISTS
 * ----------------------
 * Two callers apply the same gain: java.c's AudioTrack path and the FMOD output
 * plugin's drain thread (reimpl/fmod_output_vita.c). The plugin used to reach into
 * java.c with a function-local `extern` declaration -- a layering inversion (reimpl/
 * is meant to hold POSIX/Android reimplementations, not game-specific policy) and,
 * worse, a prototype the compiler never got to compare against the definition.
 *
 * ☠ THE SAMPLE HELPER IS `static inline` ON PURPOSE. It runs ONCE PER SAMPLE: at
 * 44100 Hz stereo that is 88,200 calls a second, and the build has no LTO, so as a
 * cross-translation-unit function every one of them was a real BL/BX pair around four
 * instructions of work. It also runs on the FMOD drain thread, which is priority 0x70
 * -- above the sim and render threads -- so its cost comes straight off the frame.
 */
#ifndef SOLOADER_AUDIO_GAIN_H
#define SOLOADER_AUDIO_GAIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Master gain in Q8 (256 = 1.0x). Read once from settings/audio_gain.txt and
 *  cached; defined in java.c. Vita hardware volume is already 0 dB, so this is the
 *  only gain control the loader has. */
int audio_gain_q8(void);

/** Apply Q8 gain to one sample, saturating.
 *
 * `>> 8` rather than `/ 256`: for a signed value those differ: division truncates
 * toward zero, so it rounds negative samples UP and positive samples DOWN, which puts
 * a small asymmetry either side of the zero crossing. An arithmetic shift floors
 * uniformly, which is what fixed-point audio normally wants -- and it is one
 * instruction instead of the add-carry-shift sequence GCC must emit to implement
 * truncating signed division. The difference is at most 1 LSB and inaudible; the
 * point is that the shift is both cheaper and the better-behaved of the two. */
static inline int16_t audio_apply_gain_i16(int16_t sample, int gain_q8) {
    const int32_t value = ((int32_t)sample * (int32_t)gain_q8) >> 8;
    if (value > 32767) {
        return (int16_t)32767;
    }
    if (value < -32768) {
        return (int16_t)-32768;
    }
    return (int16_t)value;
}

#ifdef __cplusplus
}
#endif

#endif /* SOLOADER_AUDIO_GAIN_H */
