/* EQ headroom (anti-clip): how far the 10-band EQ lifts its loudest frequency, so the signal can be turned down by that
 * much before it reaches the DSP's filters. Pure arithmetic, no libm; shared by the UI (shows the amount) and audio.c. */
#ifndef EQHEAD_H
#define EQHEAD_H

#define EQ_BANDS 10
extern const unsigned short eq_band_hz[EQ_BANDS];   /* 31 Hz .. 16 kHz, one octave apart */
#define EQ_Q_CODE 23                                /* the DSP's Q code, 32 / Q: Q = 1.39, about one octave wide */

/* peak of the combined EQ response in 0.5 dB steps, rounded up; 0 when no frequency is lifted above 0 dB.
 * db[b] = band gain in whole dB (-12..12). */
int eq_peak_half_db(const signed char db[EQ_BANDS]);

/* a gain of -half_db / 2 dB as a Q15 multiplier (32767 = unity) */
int eq_q15(int half_db);

#endif
