/* Exercise allocation and finalization failures without touching real audio. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include "../misc.h"
#ifdef _WIN32
#include <io.h>
#define REVIEW_TELL _ftelli64
#else
#define REVIEW_TELL ftello
#endif

static int fail_alloc, fail_read, fail_write, fail_seek, fail_close;
static int fail_tail_read, header_write_count;
static void *review_malloc(size_t size)
{
	if (fail_alloc) { fail_alloc = 0; errno = ENOMEM; return NULL; }
	return malloc(size);
}
static size_t review_read(void *p, size_t size, size_t count, FILE *f)
{
	if (fail_read) { fail_read = 0; return 0; }
	if (fail_tail_read && size == 1 && count == 12 && REVIEW_TELL(f) == 2044) {
		fail_tail_read = 0;
		return 0;
	}
	return fread(p, size, count, f);
}
static size_t review_write(const void *p, size_t size, size_t count, FILE *f)
{
	if (fail_write) { fail_write = 0; return 0; }
	if (header_write_count && size == 1 && count == 44 && !--header_write_count) return 0;
	return fwrite(p, size, count, f);
}
static int review_seek(FILE *f, Int64_t offset, int whence)
{
	if (fail_seek) { fail_seek = 0; errno = EIO; return -1; }
#ifdef _WIN32
	return _fseeki64(f, offset, whence);
#else
	return fseeko(f, offset, whence);
#endif
}
static int review_close(FILE *f)
{
	int result = fclose(f);
	if (fail_close) { fail_close = 0; return EOF; }
	return result;
}
#define malloc review_malloc
#define fread review_read
#define fwrite review_write
#ifdef _WIN32
#define _fseeki64 review_seek
#else
#define fseeko review_seek
#endif
#define fclose review_close
#include "../audio.c"
#undef malloc
#undef fread
#undef fwrite
#undef fseeko
#undef _fseeki64
#undef fclose
#include "../main.h"
#include "../wavegain.h"

int write_to_log;
char log_file_name[512];

static wavegain_opt *options(void)
{
	wavegain_opt *opt = calloc(1, sizeof(*opt));
	assert(opt);
	opt->channels = 1;
	opt->rate = 44100;
	opt->samplesize = 16;
	opt->format = WAV_FMT_16BIT;
	opt->gain_scale = 0.5;
	assert(prepare_wav_header(opt));
	return opt;
}

static void check_initial_failures(void)
{
	wavegain_opt *opt = options();
	audio_file *out;
	char name[] = "test/review-initial-XXXXXX";
	opt->write_chunk = 1;
	fail_alloc = 1;
	assert(!prepare_wav_header(opt));
	assert(!opt->gain_chunk && opt->header_size == 44);
	assert(prepare_wav_header(opt)); /* a failed allocation leaves a valid prefix */
	fail_write = 1;
	out = open_output_audio_file(name, opt);
	assert(!out);
	assert(fopen(name, "rb") == NULL); /* partial output has been removed */
	free(opt->header);
	free(opt);
}

static void check_finish_failure(int failure)
{
	wavegain_opt *opt = options();
	audio_file *out = calloc(1, sizeof(*out));
	FILE *in = tmpfile();
	unsigned char before[56] = {0}, after[56];
	assert(out && in);
	/* An audio prefix followed by an opaque metadata chunk. */
	memcpy(before, opt->header, 44);
	memcpy(before + 44, "LIST", 4);
	before[48] = 4;
	memcpy(before + 52, "tail", 4);
	assert(fwrite(before, 1, sizeof(before), in) == sizeof(before));
	opt->data_start = opt->data_end = 44;
	out->sndfile = tmpfile();
	assert(out->sndfile);
	out->outputFormat = WAV_FMT_16BIT;
	assert(write_wav_header(out, opt, 36));
	/* Header/metadata operations must propagate errors even without ferror(). */
	switch (failure) {
	case 1: fail_read = 1; break;
	case 2: fail_write = 1; break;
	case 3: fail_seek = 1; break;
	case 4: fail_close = 1; break;
	}
	assert(close_audio_file(in, out, opt) == (failure == 0));
	rewind(in);
	assert(fread(after, 1, sizeof(after), in) == sizeof(after));
	assert(!memcmp(before, after, sizeof(before)));
	fclose(in);
}

static void check_gain_numbers(void)
{
	unsigned char encoded[8];
	double values[] = {0., -0., 0.5, 2., 0x1p40, 0x1p-1022, 0x1p-1074, HUGE_VAL, NAN};
	size_t i;
	for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		double value;
		write_d64_le(encoded, values[i]);
		value = read_d64_le(encoded);
		assert(isnan(values[i]) ? isnan(value) : value == values[i]);
		write_d64_be(encoded, values[i]);
		value = read_d64_be(encoded);
		assert(isnan(values[i]) ? isnan(value) : value == values[i]);
	}
}

static void check_original_preserved(int failure)
{
	wavegain_opt *opt = options();
	SETTINGS settings = {0};
	double dc[2] = {0., 0.};
	unsigned char before[2056] = {0}, after[2056];
	char name[] = "test/review-original-XXXXXX";
	FILE *file = fmkstemp(name);
	assert(file);
	memcpy(before, opt->header, 44);
	put_le(before + 4, sizeof(before) - 8, 4);
	put_le(before + 20, 1, 2);
	put_le(before + 22, 1, 2);
	put_le(before + 24, 44100, 4);
	put_le(before + 28, 88200, 4);
	put_le(before + 32, 2, 2);
	put_le(before + 34, 16, 2);
	put_le(before + 40, 2000, 4);
	memcpy(before + 2044, "LIST", 4);
	before[2048] = 4;
	memcpy(before + 2052, "tail", 4);
	assert(fwrite(before, 1, sizeof(before), file) == sizeof(before));
	assert(!fclose(file));
	free(opt->header);
	free(opt);
	if (failure == 0) fail_alloc = 1;
	if (failure == 1) fail_tail_read = 1;
	if (failure == 2) header_write_count = 2; /* fail final header rewrite */
	assert(!write_gains(name, -6., 0., 0., dc, dc, &settings));
	assert(!fail_alloc && !fail_tail_read && !header_write_count);
	file = fopen(name, "rb");
	assert(file && fread(after, 1, sizeof(after), file) == sizeof(after));
	assert(!memcmp(before, after, sizeof(before)));
	fclose(file);
	remove(name);
}

int main(void)
{
	int failure;
	check_initial_failures();
	for (failure = 0; failure <= 4; failure++) check_finish_failure(failure);
	check_gain_numbers();
	for (failure = 0; failure <= 2; failure++) check_original_preserved(failure);
	puts("Review I/O: allocation, initial header, tail read/write, seek, close and gain edges passed");
	return 0;
}
