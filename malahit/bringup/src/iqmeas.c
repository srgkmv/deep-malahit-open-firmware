#include "iqmeas.h"

const int16_t iq_sin64[64] = {
         0,   1176,   2341,   3483,   4592,   5657,   6667,   7613,
      8485,   9276,   9978,  10583,  11087,  11483,  11769,  11942,
     12000,  11942,  11769,  11483,  11087,  10583,   9978,   9276,
      8485,   7613,   6667,   5657,   4592,   3483,   2341,   1176,
         0,  -1176,  -2341,  -3483,  -4592,  -5657,  -6667,  -7613,
     -8485,  -9276,  -9978, -10583, -11087, -11483, -11769, -11942,
    -12000, -11942, -11769, -11483, -11087, -10583,  -9978,  -9276,
     -8485,  -7613,  -6667,  -5657,  -4592,  -3483,  -2341,  -1176
};

/*
 * sqrt(a^2 + b^2) in integers, without overflowing.
 *
 * The accumulators below reach 8192 frames x 32768 counts x 12000 table
 * units, about 3e12, and squaring that is 9e24 - four hundred times
 * past what a uint64 holds. So scale both down until the squares fit,
 * take the root, and scale the root back up by half as many places.
 * Shifting before squaring costs at most one bit of precision in the
 * answer and the alternative is a silent wrap.
 */
static uint64_t hypot64(int64_t a, int64_t b)
{
    uint64_t x  = (a < 0) ? (uint64_t)(-a) : (uint64_t)a;
    uint64_t y  = (b < 0) ? (uint64_t)(-b) : (uint64_t)b;
    uint32_t sh = 0;

    while (x >= (1ULL << 30) || y >= (1ULL << 30)) {
        x >>= 1;
        y >>= 1;
        sh++;
    }

    uint64_t s = x * x + y * y;         /* < 2^61 */
    uint64_t r = 0;

    for (int32_t bit = 30; bit >= 0; bit--) {
        uint64_t t = r | (1ULL << bit);
        if (t * t <= s) { r = t; }
    }

    return r << sh;
}

/* log2(x) in Q16, for x > 0. Integer part from where the top bit sits,
 * fractional part by repeated squaring - sixteen iterations, one bit of
 * the fraction each. */
static int32_t log2_q16(uint64_t x)
{
    int32_t n = 0;

    while (x >= (1ULL << 31)) { x >>= 1; n++; }
    while (x <  (1ULL << 30)) { x <<= 1; n--; }

    /* x is now in [2^30, 2^31): mantissa m = x / 2^30, in [1, 2). */
    int32_t  result = (n + 30) << 16;
    uint64_t m      = x;

    for (int32_t i = 1; i <= 16; i++) {
        m = (m * m) >> 30;                  /* < 2^61 before the shift */
        if (m >= (1ULL << 31)) {
            m >>= 1;
            result += (1 << (16 - i));
        }
    }

    return result;
}

int32_t iq_db10(uint64_t num, uint64_t den)
{
    if (num == 0U) { return -9990; }
    if (den == 0U) { return  9990; }

    /*
     * 20 log10(r) = 6.0206 log2(r), and log2(num/den) is
     * log2(num) - log2(den), which avoids forming the ratio at all -
     * so a ratio of 1e-6 and a ratio of 1e6 are equally well behaved.
     *
     * 60206/65536/1000 scales Q16 log2 into tenths of a decibel.
     */
    int64_t l = (int64_t)log2_q16(num) - (int64_t)log2_q16(den);

    /* Thousandths of a decibel, then rounded to tenths. Truncating here
     * instead read 19.9 dB for a ratio of exactly ten, which is the
     * sort of error that gets copied into a table and then argued
     * about. */
    int64_t mdb = (l * 60206) >> 16;

    return (int32_t)((mdb >= 0) ? ((mdb + 500) / 1000)
                                : ((mdb - 500) / 1000));
}

int32_t iq_dbfs10(uint64_t mag, uint32_t frames)
{
    return iq_db10(mag, (uint64_t)frames * 32768U * 12000U);
}

void iq_measure(const int16_t *iq, uint32_t frames, iq_result_t *out)
{
    int64_t pr = 0, pi = 0;     /* X(+f) */
    int64_t nr = 0, ni = 0;     /* X(-f) */
    int64_t dr = 0, di = 0;     /* X(0)  */

    for (uint32_t n = 0; n < frames; n++) {
        /*
         * Slot 0 is I, slot 1 is Q - since the codec's ADCPHS bit was
         * set to put them in the order the upstream tree expects. The
         * schematic wires AUDIO_Q to the LEFT channel, so without that
         * bit this would be the other way round; see nau8822.h.
         */
        int32_t i = iq[n * 2U];
        int32_t q = iq[n * 2U + 1U];

        /* cos and sin of 2*pi*n/64, straight out of the table. cos is
         * the sine a quarter turn along, which is sixteen entries. */
        int32_t c = iq_sin64[(n + 16U) & 63U];
        int32_t s = iq_sin64[n & 63U];

        /* (i + jq) * (cos - j sin) */
        pr += (int64_t)i * c + (int64_t)q * s;
        pi += (int64_t)q * c - (int64_t)i * s;

        /* (i + jq) * (cos + j sin) */
        nr += (int64_t)i * c - (int64_t)q * s;
        ni += (int64_t)q * c + (int64_t)i * s;

        dr += i;
        di += q;
    }

    out->mag_pos = hypot64(pr, pi);
    out->mag_neg = hypot64(nr, ni);

    /*
     * The DC bin is scaled up by the table amplitude so that all three
     * magnitudes are directly comparable. The two tone bins each
     * carried a factor of 12000 from the sine table; the DC sum did
     * not, so it gets one here rather than everyone else losing theirs.
     */
    out->mag_dc = hypot64(dr * 12000, di * 12000);

    out->image_db10 = iq_db10(out->mag_pos, out->mag_neg);
    out->dc_db10    = iq_db10(out->mag_pos, out->mag_dc);
}

void iq_peak(const float *db, uint32_t bins, uint32_t fs_hz,
             iq_peak_t *out)
{
    uint32_t best  = 0;
    int      found = 0;

    /*
     * SKIP THE MIDDLE. Upstream's fft.c blanks the DC bin and its four
     * neighbours - FFT_IQ_DC_BLANK - by filling them with the average
     * of the bins four places either side. That hides the LO spike,
     * which on this board sits above the wanted carrier, but it fills
     * them with a PLAUSIBLE value rather than a small one, so a peak
     * search that trusted them could still land in the middle.
     *
     * Six either side covers the blanked five and a margin.
     */
    const uint32_t mid  = bins / 2U;
    const uint32_t skip = 6U;

    for (uint32_t k = 0; k < bins; k++) {
        if (k + skip > mid && k < mid + skip) { continue; }
        if (!found || db[k] > db[best]) { best = k; found = 1; }
    }

    out->index = best;
    out->db    = found ? db[best] : -200.0f;

    int32_t off = (int32_t)best - (int32_t)mid;

    out->offset_hz = (int32_t)(((int64_t)off * (int64_t)fs_hz)
                               / (int64_t)bins);
}
