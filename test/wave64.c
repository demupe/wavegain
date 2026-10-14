#include <assert.h>
#include "../audio.c"
#include "../w64.c"

static void tag_guid(unsigned char *guid, const char *tag)
{
    memcpy(guid, wave64, 16);
    memcpy(guid, tag, 4);
}

static FILE *fixture(Uint64_t bytes, int bits, int floating, int extensible)
{
    FILE *file = tmpfile();
    unsigned char header[40] = {0}, guid[16], fmt[40] = {0}, zero[8] = {0};
    Uint64_t end;
    assert(file);
    memcpy(header, riff64, 16);
    memcpy(header + 24, wave64, 16);
    assert(fwrite(header, 1, 40, file) == 40);
    /* Odd-size unknown GUID chunk exercises padding and metadata preservation. */
    tag_guid(guid, "junk");
    assert(write_chunk(file, guid, (const unsigned char *)"X", 1));
    put16(fmt, extensible ? WAVE_FORMAT_EXTENSIBLE : floating ? 3 : 1);
    put16(fmt + 2, 1);
    put32(fmt + 4, 44100);
    put32(fmt + 8, 44100 * bits / 8);
    put16(fmt + 12, bits / 8);
    put16(fmt + 14, bits);
    if (extensible) {
        put16(fmt + 16, 22);
        put16(fmt + 18, bits);
        put32(fmt + 20, 4);
        memcpy(fmt + 24, floating ? ieee_float_guid : pcm_guid, 16);
    }
    tag_guid(guid, "fmt ");
    assert(write_chunk(file, guid, fmt, extensible ? 40 : 16));
    tag_guid(guid, "data");
    memcpy(header, guid, 16);
    put64(header + 16, bytes + 24);
    assert(fwrite(header, 1, 24, file) == 24);
    if (bytes) {
        assert(W64_SEEK(file, bytes - 1, SEEK_CUR) == 0);
        assert(fputc(0, file) != EOF);
    }
    assert(fwrite(zero, 1, (8 - bytes % 8) % 8, file) == (8 - bytes % 8) % 8);
    tag_guid(guid, "bext");
    assert(write_chunk(file, guid, (const unsigned char *)"metadata", 8));
    end = W64_TELL(file);
    put64(header, end);
    assert(W64_SEEK(file, 16, SEEK_SET) == 0);
    assert(fwrite(header, 1, 8, file) == 8);
    rewind(file);
    return file;
}

static void roundtrip(Uint64_t bytes, int bits, int floating, int extensible)
{
    FILE *in = fixture(bytes, bits, floating, extensible);
    wavegain_opt opt = {0}, again = {0};
    audio_file out = {0};
    unsigned char header[40], tail[32];
    double value[4], *buffer[] = {value};
    assert(open_audio_file(in, &opt));
    assert(opt.is_wave64);
    assert(opt.total_samples_per_channel == bytes / (bits / 8));
    assert(opt.format == (floating ? WAV_FMT_FLOAT : bits / 8));
    if (bytes > 4ULL * 1024 * 1024 * 1024) {
        /* Mono 8-bit fixture has more than 2^32 frames. */
        assert(opt.total_samples_per_channel > UINT32_MAX);
        assert(opt.read_samples(opt.readdata, buffer, 1, 1, bytes - 1) == 1);
        assert(value[0] == -1.0);
        assert(opt.read_samples(opt.readdata, buffer, 1, 0, 0) == 0);
    }
    if (!bytes) assert(opt.read_samples(opt.readdata, buffer, 1, 0, 0) == 0);
    out.sndfile = tmpfile();
    assert(out.sndfile);
    opt.gain_scale = 0.5;
    opt.write_chunk = 1;
    assert(w64_write_header(&out, &opt));
    if (bytes) {
        assert(W64_SEEK(out.sndfile, out.data_offset + bytes - 1, SEEK_SET) == 0);
        assert(fputc(0, out.sndfile) != EOF);
    }
    out.samples = bytes / (bits / 8);
    assert(w64_finish(in, &out, &opt));
    assert(W64_TELL(out.sndfile) == out.data_offset + aligned64(bytes) + 32);
    rewind(out.sndfile);
    assert(fread(header, 1, 40, out.sndfile) == 40);
    assert(get64(header + 16) == (Uint64_t)out.data_offset + aligned64(bytes) + 32);
    rewind(out.sndfile);
    assert(open_audio_file(out.sndfile, &again));
    assert(again.total_samples_per_channel == opt.total_samples_per_channel);
    assert(again.gain_chunk && again.gain_scale == 0.5);
    assert(again.format == opt.format);
    assert(W64_SEEK(out.sndfile, 40, SEEK_SET) == 0);
    assert(fread(tail, 1, 32, out.sndfile) == 32 && tail[24] == 'X');
    assert(W64_SEEK(out.sndfile, again.data_end, SEEK_SET) == 0);
    assert(fread(tail, 1, 32, out.sndfile) == 32 && !memcmp(tail + 24, "metadata", 8));
    wav_close(opt.readdata);
    wav_close(again.readdata);
    fclose(in);
    fclose(out.sndfile);
}

static void malformed(void)
{
    unsigned char header[40];
    unsigned int sizes[] = {0, 23, 1000000};
    size_t i;
    for (i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        FILE *file = fixture(8, 16, 0, 0);
        wavegain_opt opt = {0};
        assert(fread(header, 1, 40, file) == 40);
        assert(W64_SEEK(file, 56, SEEK_SET) == 0);
        {
            unsigned char encoded[8];
            put64(encoded, sizes[i]);
            assert(fwrite(encoded, 1, 8, file) == 8);
        }
        assert(W64_SEEK(file, 40, SEEK_SET) == 0);
        assert(!w64_open(file, &opt, header, 40));
        fclose(file);
    }
    {
        FILE *file = fixture(8, 16, 0, 0);
        wavegain_opt opt = {0};
        assert(fread(header, 1, 40, file) == 40);
        put64(header + 16, UINT64_MAX);
        assert(!w64_open(file, &opt, header, 40));
        fclose(file);
    }
}

static void incomplete_output(void)
{
    FILE *in = fixture(16, 16, 0, 0);
    wavegain_opt opt = {0};
    audio_file out = {0};
    assert(open_audio_file(in, &opt));
    out.sndfile = tmpfile();
    assert(out.sndfile);
    assert(w64_write_header(&out, &opt));
    assert(!w64_finish(in, &out, &opt));
    wav_close(opt.readdata);
    fclose(in);
    fclose(out.sndfile);
}

int main(void)
{
    roundtrip(15, 24, 0, 0);
    roundtrip(0, 16, 0, 0);
    roundtrip(16, 32, 1, 0);
    roundtrip(12, 24, 0, 1);
    roundtrip(16, 32, 1, 1);
    roundtrip(5ULL * 1024 * 1024 * 1024, 8, 0, 0);
    malformed();
    incomplete_output();
    puts("Wave64: PCM/float/extensible, metadata, padding, gain, invalid sizes, and sparse 5 GiB I/O passed");
    return 0;
}
