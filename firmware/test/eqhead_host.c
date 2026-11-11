/* Host test for eqhead.c: the anti-clip headroom against a double-precision reference of the same cascaded peaking
 * filters on a much finer grid, for single bands, the UI presets and extreme settings. Exit status 0 = all passed. */
#include "../eqhead.c"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c))                                                        \
            fails++, printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
    } while (0)

static double ref_peak_db(const signed char *db)
{
    double q = 32.0 / EQ_Q_CODE, peak = 0;
    for (double f = 10; f <= 24000; f *= pow(2, 1.0 / 192)) {
        double s = 0;
        for (int b = 0; b < EQ_BANDS; b++) {
            double a = pow(10, db[b] / 40.0), w = f / eq_band_hz[b], u = 1 - w * w;
            s += 10 * log10((u * u + pow(w * a / q, 2)) / (u * u + pow(w / (a * q), 2)));
        }
        if (s > peak)
            peak = s;
    }
    return peak;
}

static void check(const char *name, const signed char *db)
{
    int got = eq_peak_half_db(db);
    double ref = ref_peak_db(db);
    int want = (int)ceil(ref * 2 - 1e-6);
    printf("%-8s peak %+6.2f dB  cut %4.1f dB\n", name, ref, got / 2.0);
    CHECK(got >= want - 1 && got <= want);   /* the coarser grid may land up to one step under the true peak */
    CHECK(got / 2.0 >= ref - 0.5);
}

int main(void)
{
    static const signed char flat[EQ_BANDS] = { 0 }, cuts[EQ_BANDS] = { -12, -6, -3, 0, -1, -2, -12, -12, -5, -1 };
    static const signed char presets[][EQ_BANDS] = { { 6, 5, 4, 2, 0, 0, 0, 0, 0, 0 },     { 5, 4, 2, 0, -1, -1, 0, 2, 4, 5 },
                                                     { -2, -1, 0, 1, 3, 4, 3, 1, 0, -1 }, { 0, 0, 0, 0, 0, 0, 1, 3, 5, 6 },
                                                     { 12, 12, 12, 12, 12, 12, 12, 12, 12, 12 } };
    static const char *const pn[] = { "Bass", "Loud", "Voice", "Treble", "all+12" };
    CHECK(eq_peak_half_db(flat) == 0);
    CHECK(eq_peak_half_db(cuts) == 0);
    for (int b = 0; b < EQ_BANDS; b++) {   /* one band alone peaks at exactly its own gain */
        signed char one[EQ_BANDS] = { 0 };
        one[b] = 6;
        CHECK(eq_peak_half_db(one) == 12);
        one[b] = 12;
        CHECK(eq_peak_half_db(one) == 24);
    }
    for (int i = 0; i < 5; i++)
        check(pn[i], presets[i]);
    CHECK(eq_q15(0) == 32767);
    CHECK(abs(eq_q15(12) - (int)(32767 * pow(10, -0.3))) <= 2);   /* 6 dB: about half */
    CHECK(abs(eq_q15(48) - (int)(32767 * pow(10, -1.2))) <= 2);
    printf(fails ? "eqhead: %d FAILED\n" : "eqhead: all passed\n", fails);
    return fails != 0;
}
