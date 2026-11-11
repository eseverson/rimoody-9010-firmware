/* EQ headroom (eqhead.h). Each band is the DSP's peaking filter (notes/audio_eq.md: type 3, gain, Q code 32/Q). The
 * response is taken from the analog prototype the digital filter is designed from (RBJ cookbook peaking EQ):
 *   |H(jw)|^2 = ((1 - w^2)^2 + (w A / Q)^2) / ((1 - w^2)^2 + (w / (A Q))^2),  w = f / f0,  A = 10^(dB / 40)
 * The bands are cascaded, so their power responses multiply. The product is evaluated on a 1/24-octave grid from 16 Hz
 * to 20 kHz and the largest value is converted to 0.5 dB steps, rounded up so the cut is never short. Bilinear warping
 * only narrows the 16 kHz band near Nyquist and does not raise its peak. */
#include "eqhead.h"

const unsigned short eq_band_hz[EQ_BANDS] = { 31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

static float a_of(int db)   /* 10^(db / 40) by repeated multiplication: 10^(1/40) = 1.0592537 */
{
    float a = 1.0f, k = db >= 0 ? 1.0592537f : 1.0f / 1.0592537f;
    for (int i = db >= 0 ? db : -db; i > 0; i--)
        a *= k;
    return a;
}

int eq_peak_half_db(const signed char db[EQ_BANDS])
{
    const float q = 32.0f / EQ_Q_CODE;
    float a[EQ_BANDS];
    int any = 0;
    for (int b = 0; b < EQ_BANDS; b++) {
        int d = db[b] > 12 ? 12 : db[b] < -12 ? -12 : db[b];
        a[b] = a_of(d);
        any |= d > 0;
    }
    if (!any)
        return 0;   /* only cuts: nothing rises above 0 dB */
    float peak = 1.0f;
    for (float f = 16.0f; f <= 20000.0f; f *= 1.0293022f) {   /* 2^(1/24) */
        float p = 1.0f;
        for (int b = 0; b < EQ_BANDS; b++) {
            if (a[b] == 1.0f)
                continue;
            float w = f / eq_band_hz[b], u = 1.0f - w * w, u2 = u * u;
            float num = w * a[b] / q, den = w / (a[b] * q);
            p *= (u2 + num * num) / (u2 + den * den);
        }
        if (p > peak)
            peak = p;
    }
    int half = 0;
    while (peak > 1.0001f && half < 96) {   /* power ratio per 0.5 dB: 10^(0.05) = 1.1220185 */
        peak /= 1.1220185f;
        half++;
    }
    return half;
}

int eq_q15(int half_db)
{
    float g = 32767.0f;
    for (int i = 0; i < half_db; i++)
        g *= 0.94406088f;   /* 10^(-0.5 / 20) */
    return (int)(g + 0.5f);
}
