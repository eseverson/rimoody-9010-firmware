/* Audio: the SoC's audio DSP brought up the way stock eCos does it (audio.c, notes/audio_dsp.md), PCM out through the
 * internal DAC. Net task only. */
#ifndef AUDIO_H
#define AUDIO_H
typedef int (*aud_fmt)(char *, const char *, ...);
typedef void (*aud_logf)(const char *, ...);

int aud_init(aud_logf log);              /* clocks, codec, DSP microcode and boot, output on, master muted; 0 = ok */
int aud_ready(void);
void aud_poll(void);                     /* every net-loop pass: DSP messages, heartbeat, keep the PCM ring fed */
void aud_set_volume(int index);          /* 0..40, eCos's volume index (0 = silent, 40 = 0 dB) */
void aud_set_mute(int on);               /* DSP master mute (the MCU's system mute is separate) */
void aud_tone(int hz);                   /* test tone into the PCM ring (0 = off): -42 dBFS, 2 s */
void aud_tone_ex(int hz, int peak, int ms);   /* peak 1..8192 (of 32767), stops by itself after ms (<= 10 s) */
int aud_write(const short *lr, int frames);   /* media: 48 kHz s16 stereo, little-endian; returns frames taken */
int aud_write_mono16k(int src, const short *m, int samples);   /* 1 guidance/speech, 2 system: 16 kHz mono, mixed in */
#define AUD_EQ_BANDS 10                  /* 31, 63, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz */
void aud_eq_set(int band, int half_db);  /* gain in 0.5 dB steps, -24..24 (+-12 dB); kept and re-applied after an init */
void aud_set_pregain(int cut_half_db);   /* anti-clip: turn media, guidance and system sounds down by this (0.5 dB steps) */
int aud_status(char *o, aud_fmt fmt);
/* microphone: the DSP's hands-free uplink (HF_ENC, ring 6), 16 kHz mono s16 little-endian, as stock AA records it.
 * Playback, routing and the EQ are not touched. While open and nobody reads for 1 s, aud_poll drains the ring (the
 * level meter stays live). aud_init restarts capture if it was open. */
int aud_mic_start(void);                 /* 0, or < 0 for the step that failed (logged) */
void aud_mic_stop(void);                 /* stop HF_ENC, ring pointers to 0 */
int aud_mic_on(void);
int aud_mic_read(short *buf, int max_samples);   /* whole 8 ms frames (128 samples) only; returns samples copied */
int aud_mic_status(char *o, aud_fmt fmt);   /* HF_ENC state, ring pointers, byte counts, level (RMS / peak) */
#endif
