#include "frame.h"

static const char HEX[] = "0123456789ABCDEF";

static size_t put_u8(char *p, uint8_t v)
{
    p[0] = HEX[(v >> 4) & 0x0Fu];
    p[1] = HEX[v & 0x0Fu];
    return 2u;
}

static size_t put_u16le(char *p, uint16_t v)
{
    size_t n = put_u8(p, (uint8_t)(v & 0xFFu));
    return n + put_u8(p + n, (uint8_t)(v >> 8));
}

static size_t put_u32le(char *p, uint32_t v)
{
    size_t n = 0u;
    for (uint32_t i = 0u; i < 4u; i++) {
        n += put_u8(p + n, (uint8_t)((v >> (8u * i)) & 0xFFu));
    }
    return n;
}

/* Big-endian hex, for the header fields a human reads left to right. */
static size_t put_hex_be(char *p, uint32_t v, uint32_t digits)
{
    for (uint32_t i = 0u; i < digits; i++) {
        p[i] = HEX[(v >> (4u * (digits - 1u - i))) & 0x0Fu];
    }
    return digits;
}

uint32_t frame_skew_us(const lsm_frame_t *f)
{
    uint32_t lo = 0xFFFFFFFFu, hi = 0u;

    for (uint32_t i = 0u; i < 16u; i++) {
        if (((f->present >> i) & 1u) == 0u) { continue; }
        uint32_t t = f->s[i].timestamp;
        if (t < lo) { lo = t; }
        if (t > hi) { hi = t; }
    }
    return (hi >= lo) ? ((hi - lo) * 25u) : 0u;
}

size_t frame_encode(const lsm_frame_t *f, uint32_t seq, bool was_still,
                    uint16_t tapped, const lsm_still_t *st, uint32_t uptime_ms,
                    uint32_t skew_us, char *out, size_t cap)
{
    size_t need = 58u + ((size_t)f->count * FRAME_RECORD_DIGITS) + 1u;
    size_t n = 0u;

    if (cap < need) { return 0u; }

    out[n++] = 'M'; out[n++] = 'S'; out[n++] = '1'; out[n++] = ' ';
    n += put_hex_be(out + n, seq, 8u);        out[n++] = ' ';
    out[n++] = was_still ? 'S' : 'M';         out[n++] = ' ';
    n += put_hex_be(out + n, f->present, 4u); out[n++] = ' ';
    n += put_hex_be(out + n, skew_us, 8u);    out[n++] = ' ';
    n += put_hex_be(out + n, f->waits, 4u);   out[n++] = ' ';
    n += put_hex_be(out + n, tapped, 4u);     out[n++] = ' ';
    n += put_hex_be(out + n, uptime_ms, 8u);  out[n++] = ' ';
    n += put_hex_be(out + n, st->attempts,   4u); out[n++] = ' ';
    n += put_hex_be(out + n, st->last_worst, 4u); out[n++] = ' ';

    for (uint32_t i = 0u; i < 16u; i++) {
        if (((f->present >> i) & 1u) == 0u) { continue; }
        const lsm_sample_t *s = &f->s[i];

        for (uint32_t k = 0u; k < 3u; k++) {
            n += put_u16le(out + n, (uint16_t)s->accel[k]);
        }
        for (uint32_t k = 0u; k < 3u; k++) {
            n += put_u16le(out + n, (uint16_t)s->gyro[k]);
        }
        n += put_u16le(out + n, (uint16_t)s->temp);
        n += put_u8(out + n, (uint8_t)s->freq_fine);
        n += put_u8(out + n, s->status);
        n += put_u32le(out + n, s->timestamp);
    }

    out[n++] = '\n';
    return n;
}
