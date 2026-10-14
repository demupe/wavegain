/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Sony Wave64 container support. PCM codecs are shared with the WAV reader.
 * Sizes include the 24-byte chunk header; padding is excluded from sizes.
 * Reference: Sony/VCS Wave64 specification, and FFmpeg libavformat/w64.c.
 */
#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"

#ifdef _WIN32
#define W64_SEEK _fseeki64
#define W64_TELL _ftelli64
#else
#define W64_SEEK fseeko
#define W64_TELL ftello
#endif

static const unsigned char riff64[16] = {
    'r','i','f','f',0x2e,0x91,0xcf,0x11,0xa5,0xd6,0x28,0xdb,4,0xc1,0,0
};
static const unsigned char wave64[16] = {
    'w','a','v','e',0xf3,0xac,0xd3,0x11,0x8c,0xd1,0,0xc0,0x4f,0x8e,0xdb,0x8a
};
/* Private WaveGain GUID: e217d848-96be-4ac0-b56a-8b46289c71e0.
 * Its payload is one little-endian IEEE-754 double, the applied scale. */
static const unsigned char gain64[16] = {
    0x48,0xd8,0x17,0xe2,0xbe,0x96,0xc0,0x4a,0xb5,0x6a,0x8b,0x46,0x28,0x9c,0x71,0xe0
};

static Uint64_t get64(const unsigned char *p)
{
    Uint64_t n = 0;
    int i;
    for (i = 7; i >= 0; i--) n = (n << 8) | p[i];
    return n;
}

static void put64(unsigned char *p, Uint64_t n)
{
    int i;
    for (i = 0; i < 8; i++, n >>= 8) p[i] = (unsigned char)n;
}

static void put32(unsigned char *p, uint32_t n)
{
    int i;
    for (i = 0; i < 4; i++, n >>= 8) p[i] = (unsigned char)n;
}

static void put16(unsigned char *p, unsigned int n)
{
    p[0] = (unsigned char)n;
    p[1] = (unsigned char)(n >> 8);
}

static Uint64_t aligned64(Uint64_t n) { return (n + 7) & ~(Uint64_t)7; }

static int chunk_is(const unsigned char *guid, const char *tag)
{
    return !memcmp(guid, tag, 4) && !memcmp(guid + 4, wave64 + 4, 12);
}

static int read_chunk(FILE *in, Uint64_t pos, Uint64_t end,
                      unsigned char *header, Uint64_t *size)
{
    if (pos > end || end - pos < 24 || fread(header, 1, 24, in) != 24) return 0;
    *size = get64(header + 16);
    return *size >= 24 && *size <= end - pos &&
           *size <= (Uint64_t)INT64_MAX - 7 && aligned64(*size) <= end - pos;
}

static int skip64(FILE *in, Uint64_t n)
{
    unsigned char buffer[65536];
    if (!W64_SEEK(in, (Int64_t)n, SEEK_CUR)) return 1;
    while (n) {
        size_t count = n > sizeof(buffer) ? sizeof(buffer) : (size_t)n;
        if (fread(buffer, 1, count, in) != count) return 0;
        n -= count;
    }
    return 1;
}

static int copy64(FILE *in, FILE *out, Uint64_t n)
{
    unsigned char buffer[65536];
    while (n) {
        size_t count = n > sizeof(buffer) ? sizeof(buffer) : (size_t)n;
        if (fread(buffer, 1, count, in) != count ||
            fwrite(buffer, 1, count, out) != count) return 0;
        n -= count;
    }
    return 1;
}

int w64_id(unsigned char *buf, int len)
{
    return len >= 40 && !memcmp(buf, riff64, 16) && !memcmp(buf + 24, wave64, 16);
}

int w64_open(FILE *in, wavegain_opt *opt, unsigned char *buf, int len)
{
    Uint64_t end, pos = 40, size;
    unsigned char header[24], fmt[40] = {0};
    size_t fmt_size = 0;
    if (!w64_id(buf, len)) return 0;
    end = get64(buf + 16);
    if (end < 40 || end > (Uint64_t)INT64_MAX) goto invalid;
    if (!opt->std_in) {
        if (W64_SEEK(in, 0, SEEK_END) || W64_TELL(in) < (Int64_t)end ||
            W64_SEEK(in, 40, SEEK_SET)) goto invalid;
    }
    while (pos < end) {
        if (!read_chunk(in, pos, end, header, &size)) goto invalid;
        if (chunk_is(header, "fmt ")) {
            if (fmt_size || size < 40) goto invalid;
            fmt_size = size - 24 > sizeof(fmt) ? sizeof(fmt) : (size_t)(size - 24);
            if (fread(fmt, 1, fmt_size, in) != fmt_size ||
                !skip64(in, aligned64(size) - 24 - fmt_size)) goto invalid;
        } else if (!memcmp(header, gain64, 16)) {
            unsigned char scale[8];
            uint64_t bits;
            if (size != 32 || opt->gain_chunk || fread(scale, 1, 8, in) != 8) goto invalid;
            bits = get64(scale);
            memcpy(&opt->gain_scale, &bits, sizeof(bits));
            if (!(opt->gain_scale > 0.0) || opt->gain_scale > 1e100) goto invalid;
            opt->gain_chunk = 1;
        } else if (chunk_is(header, "data")) {
            Uint64_t payload = size - 24;
            unsigned int block;
            if (!fmt_size) goto invalid;
            block = (fmt[2] | (fmt[3] << 8)) * ((fmt[14] | (fmt[15] << 8)) / 8);
            if (!block) goto invalid;
            opt->is_wave64 = 1;
            opt->source = in;
            opt->data_start = (Int64_t)(pos + 24);
            opt->data_end = (Int64_t)(pos + aligned64(size));
            opt->container_end = (Int64_t)end;
            if (!setup_wav(in, opt, fmt, fmt_size, (Int64_t)payload)) goto invalid;
            /* Empty data must not fall back to counting trailing metadata. */
            opt->total_samples_per_channel = payload / block;
            ((wavfile *)opt->readdata)->totalsamples = payload / block;
            ((wavfile *)opt->readdata)->data_start = opt->data_start;
            return 1;
        } else if (!skip64(in, aligned64(size) - 24)) goto invalid;
        pos += aligned64(size);
    }
invalid:
    fprintf(stderr, "Error: invalid or unsupported Sony Wave64 file\n");
    return 0;
}

static int write_chunk(FILE *out, const unsigned char *guid,
                       const unsigned char *data, size_t n)
{
    unsigned char header[24], zero[8] = {0};
    size_t pad = (8 - n % 8) % 8;
    memcpy(header, guid, 16);
    put64(header + 16, n + 24);
    return fwrite(header, 1, 24, out) == 24 &&
           fwrite(data, 1, n, out) == n && fwrite(zero, 1, pad, out) == pad;
}

static int output_fmt(FILE *out, wavegain_opt *opt)
{
    unsigned char guid[16], fmt[16] = {0};
    memcpy(guid, wave64, 16);
    memcpy(guid, "fmt ", 4);
    put16(fmt, opt->format == WAV_FMT_FLOAT ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM);
    put16(fmt + 2, opt->channels);
    put32(fmt + 4, (uint32_t)opt->rate);
    put32(fmt + 8, (uint32_t)(opt->rate * opt->channels * (opt->samplesize / 8)));
    put16(fmt + 12, opt->channels * (opt->samplesize / 8));
    put16(fmt + 14, opt->samplesize);
    return write_chunk(out, guid, fmt, sizeof(fmt));
}

/* Two passes allow exact headers on stdout and bounded-memory metadata copies. */
int w64_write_header(audio_file *out, wavegain_opt *opt)
{
    Uint64_t pos, size, header_size = 40, total, bytes, trailing;
    unsigned char header[40], guid[16], fact[8];
    int pass, have_fact = opt->format == WAV_FMT_FLOAT;
    const int have_gain = opt->gain_chunk || opt->write_chunk;
    const Uint64_t block = opt->channels * (opt->samplesize / 8);
    const Int64_t saved = W64_TELL(opt->source);
    int ok = 0;
    if (!block || opt->total_samples_per_channel > ((Uint64_t)INT64_MAX - 7) / block)
        return 0;
    bytes = opt->total_samples_per_channel * block;
    trailing = opt->container_end - opt->data_end;
    for (pass = 0; pass < 2; pass++) {
        if (W64_SEEK(opt->source, 40, SEEK_SET)) goto done;
        if (pass) {
            if (header_size > (Uint64_t)INT64_MAX - aligned64(bytes) ||
                trailing > (Uint64_t)INT64_MAX - header_size - aligned64(bytes)) goto done;
            total = header_size + aligned64(bytes) + trailing;
            memcpy(header, riff64, 16);
            put64(header + 16, total);
            memcpy(header + 24, wave64, 16);
            if (fwrite(header, 1, 40, out->sndfile) != 40) goto done;
        }
        pos = 40;
        while (pos < (Uint64_t)opt->data_start - 24) {
            if (!read_chunk(opt->source, pos, opt->container_end, header, &size)) goto done;
            if (chunk_is(header, "fmt ")) {
                if (!pass) header_size += 40;
                else if (!output_fmt(out->sndfile, opt)) goto done;
                if (!skip64(opt->source, aligned64(size) - 24)) goto done;
            } else if (chunk_is(header, "fact") || !memcmp(header, gain64, 16)) {
                if (chunk_is(header, "fact")) have_fact = 1;
                if (!skip64(opt->source, aligned64(size) - 24)) goto done;
            } else {
                if (!pass) {
                    header_size += aligned64(size);
                    if (!skip64(opt->source, aligned64(size) - 24)) goto done;
                } else if (fwrite(header, 1, 24, out->sndfile) != 24 ||
                           !copy64(opt->source, out->sndfile, aligned64(size) - 24)) goto done;
            }
            pos += aligned64(size);
        }
        if (!pass) {
            header_size += 24 + (have_fact ? 32 : 0) + (have_gain ? 32 : 0);
        } else {
            memcpy(guid, wave64, 16);
            if (have_fact) {
                memcpy(guid, "fact", 4);
                put64(fact, opt->total_samples_per_channel);
                if (!write_chunk(out->sndfile, guid, fact, 8)) goto done;
            }
            if (have_gain) {
                unsigned char scale[8];
                uint64_t bits;
                memcpy(&bits, &opt->gain_scale, sizeof(bits));
                put64(scale, bits);
                if (!write_chunk(out->sndfile, gain64, scale, 8)) goto done;
            }
            memcpy(guid, "data", 4);
            memcpy(header, guid, 16);
            put64(header + 16, bytes + 24);
            if (fwrite(header, 1, 24, out->sndfile) != 24) goto done;
        }
    }
    out->data_offset = (Int64_t)header_size;
    out->expected_bytes = bytes;
    ok = 1;
done:
    if (W64_SEEK(opt->source, saved, SEEK_SET)) ok = 0;
    return ok;
}

int w64_finish(FILE *in, audio_file *out, wavegain_opt *opt)
{
    unsigned char zero[8] = {0};
    const Uint64_t bytes = out->samples * (opt->samplesize / 8);
    const size_t pad = (8 - bytes % 8) % 8;
    if (bytes != out->expected_bytes || fwrite(zero, 1, pad, out->sndfile) != pad ||
        W64_SEEK(in, opt->data_end, SEEK_SET) ||
        !copy64(in, out->sndfile, opt->container_end - opt->data_end)) return 0;
    return !ferror(out->sndfile);
}
