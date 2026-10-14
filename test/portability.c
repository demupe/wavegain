/* Regression tests for LP64 PCM corruption and offsets beyond 2 GiB.
 * Including audio.c also exercises its private chunk-decoding helpers. */
#include <assert.h>
#include "../audio.c"

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
    fclose(out.sndfile);
}

static void check_large_wav(void)
{
    /* A sparse 3 GiB WAV requires only a few disk blocks. */
    const Int64_t data_size = 3LL * 1024 * 1024 * 1024;
    const Int64_t sample_offset = 44 + data_size - 4;
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
    assert(FSEEK64(in, sample_offset, SEEK_SET) == 0);
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

int main(void)
{
    check_pcm(WAV_FMT_24BIT, 3);
    check_pcm(WAV_FMT_32BIT, 4);
    check_gain();
    check_large_wav();
    puts("PCM, gain chunk, and sparse 3 GiB WAV tests passed");
    return 0;
}
