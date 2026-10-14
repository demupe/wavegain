/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Regression tests for LP64 PCM corruption and offsets beyond 2 GiB.
 * Including audio.c also exercises its private chunk-decoding helpers. */
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
static int fail_growth, growth_calls;
static void *checked_realloc(void *ptr, size_t size)
{
    growth_calls++;
    if (fail_growth) { errno = ENOMEM; return NULL; }
    return realloc(ptr, size);
}
#define realloc checked_realloc
#include "../audio.c"
#undef realloc

static void check_pcm(int format, unsigned int bytes)
{
    double left[] = {0, 1, -1, 123456, -654321, 8388607, -8388608};
    double right[] = {-8388608, 8388607, -654321, 123456, -1, 1, 0};
    double *input[] = {left, right};
    int32_t pcm[14];
    audio_file out = {0};
    unsigned char encoded[56];
    unsigned int i, j;

    if (format == WAV_FMT_32BIT) {
        left[5] = INT32_MAX;
        left[6] = INT32_MIN;
        right[0] = INT32_MIN;
        right[1] = INT32_MAX;
    }
    out.sndfile = tmpfile();
    assert(out.sndfile);
    out.outputFormat = format;
    out.bits_per_sample = bytes * 8;
    output_to_PCM(input, pcm, 2, 7, format);
    assert(write_audio_file(&out, pcm, 14) == 1);
    assert(out.samples == 14);
    rewind(out.sndfile);
    assert(fread(encoded, 1, sizeof(encoded), out.sndfile) == 14 * bytes);
    for (i = 0; i < 14; i++) {
        uint32_t expected = (uint32_t)(int32_t)input[i % 2][i / 2];
        for (j = 0; j < bytes; j++)
            assert(encoded[i * bytes + j] == ((expected >> (8 * j)) & 255));
    }
    free(out.scratch);
    fclose(out.sndfile);
}

static void check_large_wav(void)
{
    /* A sparse 3 GiB WAV requires only a few disk blocks. */
    const Int64_t data_size = 3LL * 1024 * 1024 * 1024;
    /* Fast reads take byte offsets relative to the start of the audio data. */
    const Int64_t sample_offset = data_size - 4;
    unsigned char header[44] = {0};
    const unsigned char sample[] = {0, 64, 0, 192};
    wavegain_opt opt = {0};
    double left[1], right[1];
    double *buffer[] = {left, right};
    FILE *in = tmpfile();
    assert(in);
    memcpy(header, "RIFF", 4);
    WRITE_U32(header + 4, data_size + 36);
    memcpy(header + 8, "WAVEfmt ", 8);
    WRITE_U32(header + 16, 16);
    WRITE_U16(header + 20, 1);
    WRITE_U16(header + 22, 2);
    WRITE_U32(header + 24, 44100);
    WRITE_U32(header + 28, 176400);
    WRITE_U16(header + 32, 4);
    WRITE_U16(header + 34, 16);
    memcpy(header + 36, "data", 4);
    WRITE_U32(header + 40, data_size);
    assert(fwrite(header, 1, sizeof(header), in) == sizeof(header));
    assert(FSEEK64(in, 44 + sample_offset, SEEK_SET) == 0);
    assert(fwrite(sample, 1, sizeof(sample), in) == sizeof(sample));
    assert(FTELL64(in) == 44 + data_size);
    rewind(in);
    assert(open_audio_file(in, &opt));
    assert(opt.total_samples_per_channel == (unsigned long)(data_size / 4));
    assert(opt.read_samples(opt.readdata, buffer, 1, 1, sample_offset) == 1);
    assert(left[0] == 0.5 && right[0] == -0.5);
    wav_close(opt.readdata);
    fclose(in);
}

static void check_gain(void)
{
    unsigned char encoded[8];
    const unsigned char expected[] = {0, 0, 0, 0, 0, 0, 0xe0, 0x3f};
    write_d64_le(encoded, 0.5);
    assert(memcmp(encoded, expected, sizeof(expected)) == 0);
    assert(read_d64_le(encoded) == 0.5);
    write_d64_be(encoded, 0.5);
    assert(read_d64_be(encoded) == 0.5);
}

static void check_float_encoding(void)
{
    const uint32_t bits[] = {0, 0x80000000, 1, 0x80000001,
        0x007fffff, 0x807fffff, 0x00800000, 0x80800000,
        0x3f000000, 0xbf000000, 0x7f7fffff, 0xff7fffff,
        0x7f800000, 0xff800000, 0x7fc12345};
    float input[sizeof(bits) / sizeof(bits[0])];
    unsigned char expected[sizeof(bits)], actual[sizeof(bits)], fallback[sizeof(bits)];
    audio_file out = {0};
    unsigned int i;
    memcpy(input, bits, sizeof(bits));
    for (i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        uint32_t value = bits[i] == UINT32_C(0x80000000) ? 0 : bits[i];
        WRITE_U32(expected + i * 4, value);
    }
    out.sndfile = tmpfile();
    out.bits_per_sample = 32;
    assert(out.sndfile);
    assert(write_audio_float(&out, input, sizeof(input) / sizeof(input[0])) == 1);
    assert(out.samples == sizeof(input) / sizeof(input[0]));
    assert(!memcmp(input, bits, sizeof(bits))); /* Input is never changed. */
    rewind(out.sndfile);
    assert(fread(actual, 1, sizeof(actual), out.sndfile) == sizeof(actual));
    assert(!memcmp(actual, expected, sizeof(actual)));
    pack_float_le(fallback, input, sizeof(input) / sizeof(input[0]));
    assert(!memcmp(fallback, expected, sizeof(fallback)));
    free(out.scratch);
    fclose(out.sndfile);
}

static void check_buffer_reuse_and_errors(void)
{
    int32_t input[] = {1, -1, 123456, -654321};
    audio_file out = {0};
    unsigned char *saved;
    size_t capacity;
    int calls;
    out.bits_per_sample = 24;
    out.sndfile = tmpfile();
    assert(out.sndfile);
    calls = growth_calls;
    assert(write_audio_24bit(&out, input, 2) == 1);
    assert(growth_calls == calls + 1);
    saved = out.scratch;
    capacity = out.scratch_capacity;
    assert(write_audio_24bit(&out, input, 1) == 1);
    assert(out.scratch == saved && out.scratch_capacity == capacity);
    assert(growth_calls == calls + 1 && out.samples == 3);
    fail_growth = 1;
    assert(write_audio_24bit(&out, input, 4) == 0);
    fail_growth = 0;
    assert(out.scratch == saved && out.scratch_capacity == capacity);
    assert(out.samples == 3 && FTELL64(out.sndfile) == 9);
    assert(write_audio_24bit(&out, input, 4) == 1 && out.samples == 7);
    assert(out.scratch_capacity >= 12);
    assert(!output_buffer(&out, 2, SIZE_MAX)); /* multiplication overflow */
    assert(write_audio_24bit(&out, input, 0) == 0 && out.samples == 7);
    free(out.scratch);
    fclose(out.sndfile);
    out.scratch = NULL; out.scratch_capacity = 0; out.samples = 0;
    out.sndfile = fopen("test/outsox.wav", "rb"); /* Valid stream, not writable. */
    assert(out.sndfile);
    assert(write_audio_24bit(&out, input, 1) == 0 && out.samples == 0);
    free(out.scratch);
    fclose(out.sndfile);
}

/* A 3 GiB 16-bit source converted to 32-bit cannot fit the RIFF size fields. */
static void check_riff_size_limit(void)
{
    wavegain_opt opt = {0};
    char name[] = "riff_limit_XXXXXX";
    opt.channels = 2;
    opt.samplesize = 32;
    opt.header_size = 44;
    opt.format = WAV_FMT_32BIT;
    opt.total_samples_per_channel = 3ULL * 1024 * 1024 * 1024 / 4;
    assert(open_output_audio_file(name, &opt) == NULL);
    /* Rejected before the temporary file is created (mkstemp keeps XXXXXX). */
    assert(!strcmp(name, "riff_limit_XXXXXX"));
}

int main(void)
{
    check_pcm(WAV_FMT_24BIT, 3);
    check_pcm(WAV_FMT_32BIT, 4);
    check_gain();
    check_float_encoding();
    check_buffer_reuse_and_errors();
    check_large_wav();
    check_riff_size_limit();
    puts("PCM, IEEE float, reusable buffers/error handling, gain, sparse 3 GiB WAV and RIFF size limit tests passed");
    return 0;
}
