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

/* Canonical 44-byte RIFF/WAVE header: 16-bit stereo 44.1 kHz. */
static void wav_header(unsigned char *h, uint32_t riff, uint32_t data)
{
    memcpy(h, "RIFF", 4);
    WRITE_U32(h + 4, riff);
    memcpy(h + 8, "WAVEfmt ", 8);
    WRITE_U32(h + 16, 16);
    WRITE_U16(h + 20, 1);
    WRITE_U16(h + 22, 2);
    WRITE_U32(h + 24, 44100);
    WRITE_U32(h + 28, 176400);
    WRITE_U16(h + 32, 4);
    WRITE_U16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    WRITE_U32(h + 40, data);
}

static Uint64_t read_u64(const unsigned char *p)
{
    return (Uint64_t)READ_U32_LE(p) | ((Uint64_t)READ_U32_LE(p + 4) << 32);
}

/* 0xFFFFFFFF sizes (a WAV written to a pipe) are read to the end of file. */
static void check_piped_input(void)
{
    unsigned char header[44], audio[4000] = {0};
    wavegain_opt opt = {0};
    FILE *in = tmpfile();
    assert(in);
    wav_header(header, 0xffffffff, 0xffffffff);
    assert(fwrite(header, 1, 44, in) == 44 && fwrite(audio, 1, 4000, in) == 4000);
    rewind(in);
    opt.apply_gain = 1;
    assert(open_audio_file(in, &opt));
    assert(opt.total_samples_per_channel == 1000);
    assert(current_pos_t == 44 + 4000); /* no unscaled audio left as a "trailing chunk" */
    wav_close(opt.readdata);
    free(opt.header);
    fclose(in);
}

/* RF64 input: ds64 gives the data size, and the kept header becomes RIFF+JUNK. */
static void check_rf64_input(void)
{
    unsigned char head[12], ds64[36] = {0}, fmt[44], audio[4000] = {0}, list[12] = "LIST";
    wavegain_opt opt = {0};
    FILE *in = tmpfile();
    assert(in);
    memcpy(head, "RF64", 4);
    WRITE_U32(head + 4, 0xffffffff);
    memcpy(head + 8, "WAVE", 4);
    memcpy(ds64, "ds64", 4);
    WRITE_U32(ds64 + 4, 28);
    WRITE_U32(ds64 + 16, 4000); /* dataSize64; the LIST chunk follows the data */
    wav_header(fmt, 0, 0xffffffff);
    WRITE_U32(list + 4, 4);
    assert(fwrite(head, 1, 12, in) == 12 && fwrite(ds64, 1, 36, in) == 36);
    assert(fwrite(fmt + 12, 1, 32, in) == 32); /* fmt chunk + data header */
    assert(fwrite(audio, 1, 4000, in) == 4000 && fwrite(list, 1, 12, in) == 12);
    rewind(in);
    opt.apply_gain = 1;
    assert(open_audio_file(in, &opt));
    assert(opt.total_samples_per_channel == 1000);
    assert(current_pos_t == 12 + 36 + 32 + 4000);
    assert(opt.rf64 && !memcmp(opt.header, "RIFF", 4) && !memcmp(opt.header + 12, "JUNK", 4));
    wav_close(opt.readdata);
    free(opt.header);
    fclose(in);
}

/* An RF64 dataSize beyond INT64_MAX must not overflow the signed offsets:
 * a 180-byte file claiming INT64_MAX bytes is read to the end of the file. */
static void check_rf64_huge_size(void)
{
    unsigned char head[12], ds64[36] = {0}, fmt[44], audio[100] = {0};
    wavegain_opt opt = {0};
    FILE *in = tmpfile();
    assert(in);
    memcpy(head, "RF64", 4);
    WRITE_U32(head + 4, 0xffffffff);
    memcpy(head + 8, "WAVE", 4);
    memcpy(ds64, "ds64", 4);
    WRITE_U32(ds64 + 4, 28);
    WRITE_U32(ds64 + 16, 0xffffffff); /* dataSize64 = INT64_MAX */
    WRITE_U32(ds64 + 20, 0x7fffffff);
    wav_header(fmt, 0, 0xffffffff);
    assert(fwrite(head, 1, 12, in) == 12 && fwrite(ds64, 1, 36, in) == 36);
    assert(fwrite(fmt + 12, 1, 32, in) == 32 && fwrite(audio, 1, 100, in) == 100);
    rewind(in);
    opt.apply_gain = 1;
    assert(open_audio_file(in, &opt));
    assert(opt.total_samples_per_channel == 25);
    assert(current_pos_t == 180);
    wav_close(opt.readdata);
    free(opt.header);
    fclose(in);
}

/* WAV output over 4 GiB is written, not rejected: by default with 0xFFFFFFFF
 * sizes (as earlier releases did), and as RF64 with --rf64. Both read back. */
/* junk: payload size of a JUNK chunk already present after "WAVE" (0 = none).
 * Only a 28-byte one can be turned into ds64 in place; others must survive. */
static void check_large_output(int rf64, unsigned int junk)
{
    const Uint64_t bytes = 4608ULL * 1024 * 1024; /* 4.5 GiB of 16-bit stereo */
    const Int64_t source_header = 44 + (junk ? 8 + (Int64_t)junk : 0);
    unsigned char header[200];
    char name[] = "test/large_out_XXXXXX";
    wavegain_opt *opt = calloc(1, sizeof(*opt)), back = {0};
    audio_file *out;
    FILE *in = tmpfile(), *file;
    Int64_t data_offset, length;
    assert(opt && in);
    opt->header = calloc(1, (size_t)source_header);
    assert(opt->header);
    wav_header(header, 36, 0);
    memcpy(opt->header, header, 12);
    if (junk) {
        memcpy(opt->header + 12, "JUNK", 4);
        WRITE_U32(opt->header + 16, junk);
    }
    memcpy(opt->header + source_header - 32, header + 12, 32);
    opt->header_size = (int)source_header;
    opt->channels = 2;
    opt->samplesize = 16;
    opt->rate = 44100;
    opt->format = WAV_FMT_16BIT;
    opt->total_samples_per_channel = bytes / 4;
    opt->rf64 = rf64;
    out = open_output_audio_file(name, opt);
    assert(out);
    data_offset = opt->header_size;
    /* A new 36-byte placeholder is inserted unless a 28-byte JUNK is reused. */
    assert(data_offset == source_header + (rf64 && junk != 28 ? 36 : 0));
    assert(FSEEK64(out->sndfile, data_offset + (Int64_t)bytes - 1, SEEK_SET) == 0);
    assert(fputc(0, out->sndfile) != EOF);
    out->samples = bytes / 2;
    current_pos_t = 0; /* the empty input has no trailing chunks to copy */
    assert(close_audio_file(in, out, opt) == 1);
    fclose(in);

    file = fopen(name, "rb");
    assert(file);
    assert(FSEEK64(file, 0, SEEK_END) == 0);
    length = FTELL64(file);
    assert(length == data_offset + (Int64_t)bytes);
    rewind(file);
    assert(fread(header, 1, sizeof(header), file) == sizeof(header));
    assert(READ_U32_LE(header + 4) == 0xffffffff);
    assert(READ_U32_LE(header + data_offset - 4) == 0xffffffff);
    if (rf64) {
        assert(!memcmp(header, "RF64", 4) && !memcmp(header + 12, "ds64", 4));
        assert(READ_U32_LE(header + 16) == 28);
        assert(read_u64(header + 20) == (Uint64_t)length - 8);
        assert(read_u64(header + 28) == bytes);
        assert(read_u64(header + 36) == bytes / 4);
        if (junk && junk != 28) /* the original JUNK chunk follows intact */
            assert(!memcmp(header + 48, "JUNK", 4) && READ_U32_LE(header + 52) == junk);
    } else {
        assert(!memcmp(header, "RIFF", 4));
    }
    rewind(file);
    assert(open_audio_file(file, &back));
    assert(back.total_samples_per_channel == bytes / 4);
    wav_close(back.readdata);
    fclose(file);
    assert(remove(name) == 0);
}

int main(void)
{
    check_pcm(WAV_FMT_24BIT, 3);
    check_pcm(WAV_FMT_32BIT, 4);
    check_gain();
    check_float_encoding();
    check_buffer_reuse_and_errors();
    check_large_wav();
    check_piped_input();
    check_rf64_input();
    check_rf64_huge_size();
    check_large_output(0, 0);
    check_large_output(1, 0);
    check_large_output(1, 28);  /* reused in place */
    check_large_output(1, 64);  /* kept; a new placeholder goes in front */
    puts("PCM, IEEE float, reusable buffers/error handling, gain, sparse 3 GiB WAV, piped/RF64 input and sparse 4.5 GiB oversized/RF64 output tests passed");
    return 0;
}
