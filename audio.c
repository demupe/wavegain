/* WaveGain - Filename: AUDIO.C
 *
 * Function: Essentially provides all the wave input and output routines.
 *
 *   Copyright (c) 2002 - 2005 John Edwards <john.edwards33@ntlworld.com>
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Library General Public
 *   License as published by the Free Software Foundation; either
 *   version 2 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Library General Public License for more details.
 *
 *   You should have received a copy of the GNU Library General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Portions, (c) Michael Smith <msmith@labyrinth.net.au>
 * Portions from Vorbize, (c) Kenneth Arnold <kcarnold@yahoo.com>
 * and libvorbis examples, (c) Monty <monty@xiph.org>
 *
 * AIFF/AIFC support from OggSquish, (c) 1994-1996 Monty <xiphmont@xiph.org>
 */


#include <stdint.h>
#include <float.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <math.h>

#ifdef _WIN32
#include <io.h>
#else
# ifndef __APPLE__
#  include <sys/io.h>
# endif
#endif

#include <fcntl.h>

#ifndef __APPLE__
#include <malloc.h>
#endif

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "audio.h"
#include "i18n.h"
#include "misc.h"

/* Create exclusively even if another process claims the generated name. */
#ifdef _WIN32
#include <sys/stat.h>
FILE *fmkstemp(char *name)
{
	int fd;
	FILE *stream;
	if (_mktemp_s(name, strlen(name) + 1)) return NULL;
	fd = _open(name, _O_CREAT | _O_EXCL | _O_WRONLY | _O_BINARY, _S_IREAD | _S_IWRITE);
	if (fd < 0) return NULL;
	stream = _fdopen(fd, "wb");
	if (!stream) { _close(fd); remove(name); }
	return stream;
}
#else
#include <unistd.h>
FILE *fmkstemp(char *name)
{
	int fd = mkstemp(name);
	FILE *stream;
	if (fd < 0) return NULL;
	stream = fdopen(fd, "wb");
	if (!stream) { close(fd); remove(name); }
	return stream;
}
#endif

/* Macros to read header data */
#define READ_U32_LE(buf) \
	(((uint32_t)(buf)[3]<<24)|((uint32_t)(buf)[2]<<16)|((uint32_t)(buf)[1]<<8)|(uint32_t)(buf)[0])

#define READ_U16_LE(buf) \
	(((buf)[1]<<8)|((buf)[0]&0xff))

#define READ_U32_BE(buf) \
	(((uint32_t)(buf)[0]<<24)|((uint32_t)(buf)[1]<<16)|((uint32_t)(buf)[2]<<8)|(uint32_t)(buf)[3])

#define READ_U16_BE(buf) \
	(((buf)[0]<<8)|((buf)[1]&0xff))

#ifdef __APPLE__
	#define READ_D64	read_d64_be
	#define WRITE_D64	write_d64_be
#else
	#define READ_D64	read_d64_le
	#define WRITE_D64	write_d64_le
#endif

static unsigned char pcm_guid[16] =
{
	/* (00000001-0000-0010-8000-00aa00389b71) */

	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
};

static unsigned char ieee_float_guid[16] =
{
	/* (00000003-0000-0010-8000-00aa00389b71) */

	0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71
};

/* Define the supported formats here */
input_format formats[] = {
	{wav_id, 12, wav_open, wav_close, "wav", N_("WAV file reader")},
	{aiff_id, 12, aiff_open, wav_close, "aiff", N_("AIFF/AIFC file reader")},
	{w64_id, 40, w64_open, wav_close, "w64", N_("Sony Wave64 file reader")},
	{NULL, 0, NULL, NULL, NULL, NULL}
};

#if (defined (WIN32) || defined (_WIN32))
#ifndef __MINGW32__	/* MinGW's <stdio.h> already declares them (dllimport) */
extern int __cdecl _fseeki64(FILE *, Int64_t, int);
extern Int64_t __cdecl _ftelli64(FILE *);
#endif

	#define FSEEK64		_fseeki64
	#define FTELL64		_ftelli64
#else
	#define FSEEK64		fseeko
	#define FTELL64		ftello
#endif

/* Global */
Int64_t current_pos_t;

/* Use the portable C99 lrint() from <math.h>. The former x87 fistpl
 * wrote only 32 bits into a long, leaving its upper half undefined on LP64. */

/* Decode IEEE binary64 arithmetically, including subnormals, without shifts
 * by an untrusted exponent or assumptions about the native double layout. */
static double read_d64(const unsigned char *p, int big)
{
	uint64_t bits = 0, fraction;
	unsigned int exponent;
	double value;
	int i;
	for (i = 0; i < 8; i++) bits = (bits << 8) | p[big ? i : 7 - i];
	fraction = bits & UINT64_C(0x000fffffffffffff);
	exponent = (unsigned int)((bits >> 52) & 0x7ff);
	if (exponent == 0x7ff) value = fraction ? NAN : HUGE_VAL;
	else if (!exponent) value = ldexp((double)fraction, -1074);
	else value = ldexp(1. + (double)fraction / 4503599627370496., (int)exponent - 1023);
	return bits >> 63 ? -value : value;
}

double read_d64_be(unsigned char *p) { return read_d64(p, 1); }
double read_d64_le(unsigned char *p) { return read_d64(p, 0); }

static void write_d64(unsigned char *out, double value, int big)
{
	uint64_t bits = signbit(value) ? UINT64_C(0x8000000000000000) : 0;
	int exponent, i;
	double fraction;
	value = fabs(value);
	if (isnan(value)) bits |= UINT64_C(0x7ff8000000000000);
	else if (isinf(value)) bits |= UINT64_C(0x7ff0000000000000);
	else if (value) {
		fraction = frexp(value, &exponent);
		if (exponent < -1021)
			/* Scale the normalized fraction: some Windows runtimes overflow
			 * ldexp's intermediate scale factor for an exponent of 1074. */
			bits |= (uint64_t)ldexp(fraction, exponent + 1074);
		else {
			bits |= (uint64_t)(exponent + 1022) << 52;
			bits |= (uint64_t)((fraction * 2. - 1.) * 4503599627370496.);
		}
	}
	for (i = 0; i < 8; i++, bits >>= 8) out[big ? 7 - i : i] = (unsigned char)bits;
}

void write_d64_be(unsigned char *p, double value) { write_d64(p, value, 1); }
void write_d64_le(unsigned char *p, double value) { write_d64(p, value, 0); }

input_format *open_audio_file(FILE *in, wavegain_opt *opt)
{
	int j = 0;
	unsigned char *buf = NULL;
	int buf_size=0, buf_filled = 0;
	int size, ret;

	while (formats[j].id_func) {
		size = formats[j].id_data_len;
		if (size >= buf_size) {
			unsigned char *grown = realloc(buf, (size_t)size);
			if (!grown) { free(buf); return NULL; }
			buf = grown;
			buf_size = size;
		}

		if (size > buf_filled) {
			ret = fread(buf+buf_filled, 1, buf_size-buf_filled, in);
			buf_filled += ret;

			if (buf_filled < size) {
				/* File truncated */
				j++;
				continue;
			}
		}

		if (formats[j].id_func(buf, buf_filled)) {
			/* ok, we now have something that can handle the file */
			if (formats[j].open_func(in, opt, buf, buf_filled)) {
				free(buf);
				return &formats[j];
			}
			/* A recognized but invalid container is not another format. */
			break;
		}
		j++;
	}

	free(buf);

	return NULL;
}

static int seek_forward(FILE *in, Int64_t length)
{
	if (length < 0) return 0;
	if (FSEEK64(in, length, SEEK_CUR)) {
		/* Failed. Do it the hard way. */
		unsigned char buf[1024];
		Int64_t seek_needed = length;
		size_t seeked;
		while (seek_needed > 0) {
			seeked = fread(buf, 1, seek_needed > 1024 ? 1024:seek_needed, in);
			if (!seeked)
				return 0; /* Couldn't read more, can't read file */
			else
				seek_needed -= seeked;
		}
	}
	return 1;
}


static int find_wav_chunk(FILE *in, char *type, Int64_t *len)
{
	unsigned char buf[8];

	while (1) {
		if (fread(buf,1,8,in) < 8) {
			/* Suck down a chunk specifier */
			if (memcmp(type, "gain", 4))
				fprintf(stderr, "Warning: Unexpected EOF in reading WAV header (1)\n");
			return 0; /* EOF before reaching the appropriate chunk */
		}

		if (memcmp(buf, type, 4)) {
			*len = READ_U32_LE(buf+4);
			if (!seek_forward(in, *len + (*len & 1)))
				return 0;

			buf[4] = 0;
		}
		else {
			*len = READ_U32_LE(buf+4);
			return 1;
		}
	}
	return 0; /* unreachable */
}

double read_IEEE80(unsigned char *buf)
{
	uint32_t hi = READ_U32_BE(buf + 2), lo = READ_U32_BE(buf + 6);
	int exponent = ((buf[0] & 0x7f) << 8) | buf[1];
	double value;
	if (exponent == 32767) return NAN;
	value = ldexp((double)hi, 32) + lo;
	value = ldexp(value, (exponent ? exponent : 1) - 16446);
	return buf[0] & 0x80 ? -value : value;
}

/* AIFF/AIFC support adapted from the old OggSQUISH application */
int aiff_id(unsigned char *buf, int len)
{
	if (len < 12) return 0; /* Truncated file, probably */

	if (memcmp(buf, "FORM", 4))
		return 0;

	if (memcmp(buf + 8, "AIF",3))
		return 0;

	if (buf[11] != 'C' && buf[11] != 'F')
		return 0;

	return 1;
}

int aiff_open(FILE *in, wavegain_opt *opt, unsigned char *buf,
              int buflen __attribute__((unused)))
{
	unsigned char head[8], comm[22];
	Uint64_t remaining = READ_U32_BE(buf + 4), size, padded, frames = 0;
	int have_comm = 0, channels = 0, big = BIG;
	const int aifc = buf[11] == 'C';
	double rate = 0.;
	Int64_t start = FTELL64(in), actual;
	if (remaining < 4) goto invalid;
	/* Seekable AIFF must fit within the physical file as well as FORM. */
	if (start >= 0) {
		if (FSEEK64(in, 0, SEEK_END) || (actual = FTELL64(in)) < 0 ||
		    FSEEK64(in, start, SEEK_SET) || remaining + 8 > (Uint64_t)actual)
			goto invalid;
	}
	remaining -= 4;
	while (remaining >= 8) {
		if (fread(head, 1, 8, in) != 8) goto invalid;
		remaining -= 8;
		size = READ_U32_BE(head + 4);
		padded = size + (size & 1);
		if (padded > remaining) goto invalid;
		if (!memcmp(head, "COMM", 4)) {
			size_t needed = aifc ? 22 : 18;
			if (have_comm || size < needed || fread(comm, 1, needed, in) != needed)
				goto invalid;
			channels = READ_U16_BE(comm);
			frames = READ_U32_BE(comm + 2);
			rate = read_IEEE80(comm + 8);
			/* Only the documented 16-bit AIFF input is supported. */
			if (channels < 1 || channels > 2 || READ_U16_BE(comm + 6) != 16 ||
			    !isfinite(rate) || rate < 1. || rate > INT_MAX || floor(rate) != rate)
				goto invalid;
			if (aifc && memcmp(comm + 18, "NONE", 4)) {
				if (memcmp(comm + 18, "sowt", 4)) goto invalid;
				big = LITTLE;
			}
			if (!seek_forward(in, (Int64_t)(padded - needed))) goto invalid;
			have_comm = 1;
		} else if (!memcmp(head, "SSND", 4)) {
			Uint64_t offset;
			aifffile *aiff;
			if (!have_comm || size < 8 || fread(head, 1, 8, in) != 8) goto invalid;
			offset = READ_U32_BE(head);
			if (READ_U32_BE(head + 4) || offset > size - 8 ||
			    frames > (size - 8 - offset) / (channels * 2) ||
			    !seek_forward(in, (Int64_t)offset)) goto invalid;
			aiff = calloc(1, sizeof(*aiff));
			if (!aiff) goto invalid;
			aiff->f = in;
			aiff->channels = (short)channels;
			aiff->samplesize = 16;
			aiff->bigendian = (short)big;
			aiff->totalsamples = frames;
			aiff->bounded = 1;
			aiff->data_start = FTELL64(in);
			opt->read_samples = wav_read;
			opt->readdata = aiff;
			opt->rate = (long)rate;
			opt->channels = channels;
			opt->samplesize = 16;
			opt->endianness = big;
			opt->format = aifc ? WAV_FMT_AIFC16 : WAV_FMT_AIFF;
			opt->total_samples_per_channel = frames;
			return 1;
		} else if (!seek_forward(in, (Int64_t)padded)) goto invalid;
		remaining -= padded;
	}
invalid:
	fprintf(stderr, "Error: invalid or unsupported AIFF/AIFC (requires mono/stereo 16-bit PCM).\n");
	return 0;
}

int wav_id(unsigned char *buf, int len)
{
	/*unsigned int flen = READ_U32_LE(buf + 4);*/ /* We don't use this */;

	if (len < 12) return 0; /* Something screwed up */

	if (memcmp(buf, "RIFF", 4) && memcmp(buf, "RF64", 4))
		return 0; /* Not wave (RF64 is RIFF with 64-bit sizes in ds64) */

	if (memcmp(buf + 8, "WAVE",4))
		return 0; /* RIFF, but not wave */

	return 1;
}

int valid_gain_scale(double scale)
{
	return scale > 0. && isfinite(scale) && isfinite(1. / scale);
}

int wav_open(FILE *in, wavegain_opt *opt,
             unsigned char *oldbuf,
             int buflen __attribute__((unused)))
{
	unsigned char buf[81];
	Int64_t len;
	Int64_t current_pos;
	size_t fmt_size;
	const int rf64 = !memcmp(oldbuf, "RF64", 4);
	Uint64_t rf64_data = 0;

	/* Ok. At this point, we know we have a WAV file. Now we have to detect
	 * whether we support the subtype, and we have to find the actual data
	 * We don't (for the wav reader) need to use the buffer we used to id this
	 * as a wav file (oldbuf)
	 */

	if (rf64) {
		/* RF64: the first chunk, ds64, holds the 64-bit RIFF/data sizes. */
		unsigned char ds64[28];
		if (!find_wav_chunk(in, "ds64", &len) || len < 28 ||
		    fread(ds64, 1, 28, in) < 28 || !seek_forward(in, len - 28 + (len & 1))) {
			fprintf(stderr, "Warning: Invalid ds64 chunk in RF64 header\n");
			return 0;
		}
		rf64_data = (Uint64_t)READ_U32_LE(ds64 + 8) |
		            ((Uint64_t)READ_U32_LE(ds64 + 12) << 32);
	}

	if (!find_wav_chunk(in, "fmt ", &len)) {
		fprintf(stderr, "Warning: Failed to find fmt chunk in reading WAV header\n");
		return 0; /* EOF */
	}

	if (len < 16) {
		fprintf(stderr, "Warning: Unrecognised format chunk in WAV header\n");
		return 0; /* Weird format chunk */
	}

	/* A common error is to have a format chunk that is not 16 or 18 bytes
	 * in size.  This is incorrect, but not fatal, so we only warn about 
	 * it instead of refusing to work with the file.  Please, if you
	 * have a program that's creating format chunks of sizes other than
	 * 16, 18 or 40 bytes in size, report a bug to the author.
	 * (40 bytes accommodates WAVEFORMATEXTENSIBLE conforming files.)
	 */
	if (len != 16 && len != 18 && len != 40)
		fprintf(stderr, "Warning: INVALID format chunk in WAV header.\n"
				" Trying to read anyway (may not work)...\n");

	fmt_size = len > (Int64_t)sizeof(buf) ? sizeof(buf) : (size_t)len;
	if (fread(buf, 1, fmt_size, in) != fmt_size ||
	    !seek_forward(in, len - (Int64_t)fmt_size + (len & 1))) return 0;
	if (READ_U16_LE(buf) == WAVE_FORMAT_EXTENSIBLE &&
	    (fmt_size < 18 || READ_U16_LE(buf + 16) > len - 18)) return 0;

	/* Walk actual chunk boundaries; a gain chunk need not follow fmt. */
	for (;;) {
		unsigned char head[8], encoded[8];
		if (fread(head, 1, 8, in) != 8) return 0;
		len = READ_U32_LE(head + 4);
		if (!memcmp(head, "data", 4)) break;
		if (!memcmp(head, "gain", 4)) {
			if (len != 8 || opt->gain_chunk || fread(encoded, 1, 8, in) != 8) return 0;
			opt->gain_scale = READ_D64(encoded);
			if (!valid_gain_scale(opt->gain_scale)) return 0;
			opt->gain_chunk = 1;
		} else if (!seek_forward(in, len + (len & 1))) return 0;
	}
	/* Validate the unsigned 64-bit size before it becomes a signed offset;
	 * anything beyond INT64_MAX is treated as "past the end of the file". */
	if (rf64 && len == 0xffffffff)
		len = rf64_data > (Uint64_t)INT64_MAX ? INT64_MAX : (Int64_t)rf64_data;

	/* A data size of 0xFFFFFFFF marks a WAV whose size was never written (a
	 * pipe) or that outgrew 32-bit sizes (an "oversized" WAV over 4 GiB). Read
	 * such data, or data claimed past the end of a truncated file, to EOF. */
	if (!opt->std_in) {
		Int64_t here = FTELL64(in), end = -1;
		if (here < 0 || FSEEK64(in, 0, SEEK_END) || (end = FTELL64(in)) < here)
			return 0;
		if (end >= here && here >= 0) {
			if (len > end - here) {
				if (len != 0xffffffff)
					fprintf(stderr, "Warning: WAV data chunk extends past the end of the file;"
					                " reading to the end.\n");
				len = end - here;
			}
			else if (!rf64 && len == 0xffffffff)
				len = end - here;
		}
		if (FSEEK64(in, here, SEEK_SET)) return 0;
	}
	/* Keep data start + length representable even if the file size is unknown. */
	{
		const Int64_t here = FTELL64(in);
		if (here >= 0 && len > INT64_MAX - here)
			len = INT64_MAX - here;
	}

	current_pos = FTELL64(in);
	if (current_pos >= 0) {
		opt->data_start = current_pos;
		opt->data_end = current_pos + len;
		current_pos_t = opt->data_end;
	}
	if (opt->apply_gain) {
		if (current_pos < 12 || current_pos > INT_MAX || FSEEK64(in, 0, SEEK_SET)) return 0;
		opt->header = malloc((size_t)current_pos);
		if (!opt->header) return 0;
		opt->header_size = (int)current_pos;
		if (fread(opt->header, 1, (size_t)current_pos, in) != (size_t)current_pos) return 0;
		/* The reserved RF64 chunk can be patched again on finalization. */
		if (rf64 && opt->header_size >= 48 && !memcmp(opt->header + 12, "ds64", 4) &&
		    READ_U32_LE(opt->header + 16) == 28) {
			memcpy(opt->header, "RIFF", 4);
			memcpy(opt->header + 12, "JUNK", 4);
			memset(opt->header + 20, 0, 28);
			opt->rf64 = 1;
		}
		if (FSEEK64(in, current_pos, SEEK_SET)) return 0;
	}

	return setup_wav(in, opt, buf, fmt_size, len);
}

int setup_wav(FILE *in, wavegain_opt *opt, const unsigned char *buf,
              size_t fmt_size, Int64_t len)
{
	wav_fmt format;
	int samplesize;
	if (fmt_size < 16 || len < 0) return 0;
	format.format = READ_U16_LE(buf);
	format.channels = READ_U16_LE(buf+2);
	format.samplerate = READ_U32_LE(buf+4);
	format.align = READ_U16_LE(buf+12);
	format.samplesize = READ_U16_LE(buf+14);
	if (format.channels < 1 || format.channels > 2 || !format.samplerate) return 0;
	if (format.format == WAVE_FORMAT_EXTENSIBLE &&
	    (fmt_size < 40 || READ_U16_LE(buf + 16) < 22 ||
	     READ_U16_LE(buf + 18) > format.samplesize)) return 0;
	if(format.format == WAVE_FORMAT_PCM) {
		samplesize = format.samplesize/8;
		opt->read_samples = wav_read;
		/* works with current enum */
		opt->format = samplesize;
	}
	else if(format.format == WAVE_FORMAT_IEEE_FLOAT) {
		samplesize = 4;
		opt->read_samples = wav_ieee_read;
		opt->endianness = LITTLE;
		opt->format = WAV_FMT_FLOAT;
	}
	else if (format.format == WAVE_FORMAT_EXTENSIBLE) {
		format.channel_mask = READ_U32_LE(buf+20);
		if (format.channel_mask > 3 &&
		    !(format.channels == 1 && format.channel_mask == 4)) {
			fprintf(stderr, "ERROR: Wav file is unsupported type (must be standard 1 or 2 channel PCM\n"
					" or type 3 floating point PCM)(2)\n");
			return 0;
		}	
		if (memcmp(buf+24, pcm_guid, 16) == 0) {
			samplesize = format.samplesize/8;
			opt->read_samples = wav_read;
			/* works with current enum */
			opt->format = samplesize;
		}
		else if (memcmp(buf+24, ieee_float_guid, 16) == 0) {
			if (READ_U16_LE(buf + 18) && READ_U16_LE(buf + 18) != 32) return 0;
			samplesize = 4;
			opt->read_samples = wav_ieee_read;
			opt->endianness = LITTLE;
			opt->format = WAV_FMT_FLOAT;
		}
		else {
			fprintf(stderr, "ERROR: Wav file is unsupported type (must be standard PCM\n"
					" or type 3 floating point PCM)(2)\n");
			return 0;
		}
	}
	else {
		fprintf(stderr, "ERROR: Wav file is unsupported type (must be standard PCM\n"
				" or type 3 floating point PCM\n");
		return 0;
	}

	opt->samplesize = format.samplesize;

	if(format.align != format.channels * samplesize) {
		/* This is incorrect according to the spec. Warn loudly, then ignore
		 * this value.
		 */
		fprintf(stderr, _("Warning: WAV 'block alignment' value is incorrect, ignoring.\n" 
		    "The software that created this file is incorrect.\n"));
	}
	if ( format.align == format.channels * samplesize &&
			format.samplesize == samplesize * 8 && 
			(format.samplesize == 32 || format.samplesize == 24 ||
			 format.samplesize == 16 || format.samplesize == 8)) {
		/* OK, good - we have the one supported format,
		   now we want to find the size of the file */
		if (len % (format.channels * samplesize)) {
			fprintf(stderr, "Error: audio data ends with an incomplete PCM frame.\n");
			return 0;
		}
		opt->rate = format.samplerate;
		opt->channels = format.channels;

		wavfile *wav = calloc(1, sizeof(*wav));
		if (!wav) return 0;
		wav->f = in;
		wav->samplesread = 0;
		wav->bigendian = 0;
		opt->endianness = LITTLE;
		wav->channels = format.channels; /* This is in several places. The price
							of trying to abstract stuff. */
		wav->samplesize = format.samplesize;

		opt->total_samples_per_channel = (Uint64_t)len / (format.channels * samplesize);
		wav->totalsamples = opt->total_samples_per_channel;
		wav->data_start = FTELL64(in);
		wav->bounded = 1;

		opt->readdata = (void *)wav;
		return 1;
	}
	else {
		fprintf(stderr, "ERROR: Wav file is unsupported subformat (must be 8, 16, 24 or 32 bit PCM\n"
				"or floating point PCM)\n");
		return 0;
	}
}

long wav_read(void *in, double **buffer, int samples, int fast, Int64_t chunk)
{
	wavfile *f = (wavfile *)in;
	int sampbyte = f->samplesize / 8;
	signed char *buf = alloca(samples*sampbyte*f->channels);
	long bytes_read;
	int i, j;
	long realsamples;

	if (fast) {
		/* chunk is a byte offset relative to the start of the audio data. */
		f->samplesread = chunk / (sampbyte * f->channels);
		if (FSEEK64(f->f, f->data_start +
		            (Int64_t)f->samplesread * sampbyte * f->channels, SEEK_SET))
			return -1;
	}

	if (f->bounded) {
		if (f->samplesread >= f->totalsamples) return 0;
		if ((Uint64_t)samples > f->totalsamples - f->samplesread)
			samples = (int)(f->totalsamples - f->samplesread);
	}
	bytes_read = fread(buf, 1, samples * sampbyte * f->channels, f->f);

	if (f->totalsamples && f->samplesread + bytes_read / (sampbyte * f->channels) > f->totalsamples) {
		bytes_read = sampbyte * f->channels * (f->totalsamples - f->samplesread);
	}

	realsamples = bytes_read / (sampbyte * f->channels);
	f->samplesread += realsamples;
		
	if (ferror(f->f) || bytes_read % (sampbyte * f->channels) ||
	    (!realsamples && f->bounded && f->samplesread < f->totalsamples)) return -1;
	/* Select the byte order/width once per block, not once per sample.
	 * XOR the sign bit before converting to double: this avoids unaligned
	 * loads, signed shifts and out-of-range unsigned-to-signed conversions.
	 * All integer values and power-of-two scaling are exact in binary64. */
#define DECODE_PCM(read, sign, scale) \
	do { \
		for (j = 0; j < f->channels; j++) { \
			const unsigned char *base = (const unsigned char *)buf + j * sampbyte; \
			for (i = 0; i < realsamples; i++) { \
				const unsigned char *p = base + (size_t)i * sampbyte * f->channels; \
				buffer[j][i] = ((double)((read) ^ (sign)) - (double)(sign)) * (scale); \
			} \
		} \
	} while (0)
	switch (sampbyte) {
	case 1:
		for (j = 0; j < f->channels; j++)
			for (i = 0; i < realsamples; i++)
				buffer[j][i] = ((const unsigned char *)buf)[i * f->channels + j] / 128. - 1.;
		break;
	case 2:
		if (f->bigendian) DECODE_PCM(READ_U16_BE(p), UINT32_C(0x8000), 0x1p-15);
		else DECODE_PCM(READ_U16_LE(p), UINT32_C(0x8000), 0x1p-15);
		break;
	case 3:
		if (f->bigendian)
			DECODE_PCM(((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2],
			           UINT32_C(0x800000), 0x1p-23);
		else
			DECODE_PCM(((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0],
			           UINT32_C(0x800000), 0x1p-23);
		break;
	case 4:
		if (f->bigendian) DECODE_PCM(READ_U32_BE(p), UINT32_C(0x80000000), 0x1p-31);
		else DECODE_PCM(READ_U32_LE(p), UINT32_C(0x80000000), 0x1p-31);
		break;
	}
#undef DECODE_PCM

	return realsamples;
}

long wav_ieee_read(void *in, double **buffer, int samples, int fast, Int64_t chunk)
{
	wavfile *f = (wavfile *)in;
	float *buf = alloca(samples * 4 * f->channels); /* de-interleave buffer */
	long bytes_read;
	int i,j;
	long realsamples;

	if (fast) {
		/* chunk is a byte offset relative to the start of the audio data. */
		f->samplesread = chunk / (4 * f->channels);
		if (FSEEK64(f->f, f->data_start +
		            (Int64_t)f->samplesread * 4 * f->channels, SEEK_SET))
			return -1;
	}

	if (f->bounded) {
		if (f->samplesread >= f->totalsamples) return 0;
		if ((Uint64_t)samples > f->totalsamples - f->samplesread)
			samples = (int)(f->totalsamples - f->samplesread);
	}
	bytes_read = fread(buf, 1, samples * 4 * f->channels, f->f);

	if (f->totalsamples && f->samplesread + bytes_read / (4 * f->channels) > f->totalsamples)
		bytes_read = 4 * f->channels * (f->totalsamples - f->samplesread);
	realsamples = bytes_read / (4 * f->channels);
	f->samplesread += realsamples;

	if (ferror(f->f) || bytes_read % (4 * f->channels) ||
	    (!realsamples && f->bounded && f->samplesread < f->totalsamples)) return -1;
	for (i = 0; i < realsamples; i++) {
		for (j = 0; j < f->channels; j++) {
			uint32_t bits = READ_U32_LE((unsigned char *)buf + (i * f->channels + j) * 4);
			float value;
			memcpy(&value, &bits, sizeof(value));
			buffer[j][i] = value;
		}
	}

	return realsamples;
}


void wav_close(void *info)
{
	wavfile *f = (wavfile *)info;

	free(f);
}

int raw_open(FILE *in, wavegain_opt *opt)
{
	wav_fmt format; /* fake wave header ;) */
	wavfile *wav = calloc(1, sizeof(wavfile));

	/* construct fake wav header ;) */
	format.format =      2; 
	format.channels =    opt->channels;
	format.samplerate =  opt->rate;
	format.samplesize =  opt->samplesize;
	format.bytespersec = opt->channels * opt->rate * opt->samplesize / 8;
	format.align =       format.bytespersec;
	wav->f =             in;
	wav->samplesread =   0;
	wav->bigendian =     opt->endianness;
	wav->channels =      format.channels;
	wav->samplesize =    opt->samplesize;
	wav->totalsamples =  0;
	wav->bounded = 0;

	opt->read_samples = wav_read;
	opt->readdata = (void *)wav;
	opt->total_samples_per_channel = 0; /* raw mode, don't bother */
	return 1;
}

/*
 * W A V E   O U T P U T
 */

static void put_le(unsigned char *p, Uint64_t n, int bytes)
{
	int i;
	for (i = 0; i < bytes; i++, n >>= 8) p[i] = (unsigned char)n;
}

/* Locate chunks structurally, including padding. Never search their payloads.
 * This also validates the entire saved prefix before any output is opened. */
static int wav_header_layout(const wavegain_opt *opt, size_t *fmt, size_t *gain)
{
	size_t pos = 12, end;
	*fmt = *gain = 0;
	if (!opt->header || opt->header_size < 12 || memcmp(opt->header, "RIFF", 4) ||
	    memcmp(opt->header + 8, "WAVE", 4)) return 0;
	end = (size_t)opt->header_size;
	while (pos <= end && end - pos >= 8) {
		const unsigned char *p = opt->header + pos;
		Uint64_t length = READ_U32_LE(p + 4), padded = length + (length & 1);
		if (!memcmp(p, "data", 4))
			return *fmt && pos == end - 8 && !!*gain == !!opt->gain_chunk;
		if (padded > end - pos - 8) return 0;
		if (!memcmp(p, "fmt ", 4)) {
			if (*fmt || length < 16) return 0;
			*fmt = pos;
		} else if (!memcmp(p, "gain", 4)) {
			if (*gain || length != 8) return 0;
			*gain = pos;
		}
		pos += 8 + (size_t)padded;
	}
	return 0;
}

static int prepare_wav_header(wavegain_opt *opt)
{
	size_t fmt, gain, at;
	unsigned char *grown;
	if (!opt->header) {
		opt->header = calloc(1, 44);
		if (!opt->header) return 0;
		opt->header_size = 44;
		memcpy(opt->header, "RIFF", 4);
		memcpy(opt->header + 8, "WAVEfmt ", 8);
		put_le(opt->header + 16, 16, 4);
		memcpy(opt->header + 36, "data", 4);
	}
	if (!wav_header_layout(opt, &fmt, &gain)) return 0;
	if (opt->write_chunk && !gain) {
		if (opt->header_size > INT_MAX - 16) return 0;
		/* Keep the historical insertion immediately after the real fmt chunk. */
		at = fmt + 8 + READ_U32_LE(opt->header + fmt + 4);
		at += at & 1;
		grown = malloc((size_t)opt->header_size + 16);
		if (!grown) return 0;
		memcpy(grown, opt->header, at);
		memcpy(grown + at, "gain", 4);
		put_le(grown + at + 4, 8, 4);
		memset(grown + at + 8, 0, 8);
		memcpy(grown + at + 16, opt->header + at, (size_t)opt->header_size - at);
		free(opt->header);
		opt->header = grown;
		opt->header_size += 16;
		opt->gain_chunk = 1;
	}
	return 1;
}

/* Reserve a JUNK chunk right after "WAVE" that can become ds64 when the
 * output exceeds 4 GiB (EBU Tech 3306). finish_rf64 writes a 28-byte ds64 in
 * place, so only a JUNK chunk of exactly 28 bytes (for example from RF64
 * input) is reused; any other size would break the following chunk boundary,
 * so a new placeholder is inserted in front of it instead. */
static int reserve_ds64(wavegain_opt *opt)
{
	unsigned char *grown;
	if (opt->header_size >= 48 && !memcmp(opt->header + 12, "JUNK", 4) &&
	    READ_U32_LE(opt->header + 16) == 28)
		return 1;
	if (!opt->header || opt->header_size < 12 || opt->header_size > INT_MAX - 36 ||
	    (grown = malloc((size_t)opt->header_size + 36)) == NULL)
		return 0;
	memcpy(grown, opt->header, 12);
	memcpy(grown + 12, "JUNK", 4);
	put_le(grown + 16, 28, 4);
	memset(grown + 20, 0, 28);
	memcpy(grown + 48, opt->header + 12, (size_t)opt->header_size - 12);
	free(opt->header);
	opt->header = grown;
	opt->header_size += 36;
	return 1;
}

/* Turn a finished RIFF file with a reserved JUNK chunk into RF64. */
static int finish_rf64(audio_file *aufile, wavegain_opt *opt, Int64_t file_end)
{
	unsigned char head[8], ds64[36];
	const Uint64_t data = aufile->samples * (opt->samplesize / 8);
	const Uint64_t frames = data / ((Uint64_t)opt->channels * (opt->samplesize / 8));
	memcpy(head, "RF64", 4);
	put_le(head + 4, 0xffffffff, 4);
	memcpy(ds64, "ds64", 4);
	put_le(ds64 + 4, 28, 4);
	put_le(ds64 + 8, (Uint64_t)file_end - 8, 8);
	put_le(ds64 + 16, data, 8);
	put_le(ds64 + 24, frames, 8);
	put_le(ds64 + 32, 0, 4);	/* no table entries */
	return !FSEEK64(aufile->sndfile, 0, SEEK_SET) &&
	       fwrite(head, 1, 8, aufile->sndfile) == 8 &&
	       !FSEEK64(aufile->sndfile, 12, SEEK_SET) &&
	       fwrite(ds64, 1, 36, aufile->sndfile) == 36;
}

audio_file *open_output_audio_file(char *infile, wavegain_opt *opt)
{
	/* AIFF retains 32-bit sizes for every input container. */
	if (opt->format == WAV_FMT_AIFF &&
	    opt->total_samples_per_channel > (UINT32_MAX - 46) / (opt->channels * 2)) {
		fprintf(stderr, " Error: audio exceeds the AIFF size limit.\n");
		return NULL;
	}
	if (!opt->is_wave64 && opt->format != WAV_FMT_AIFF && !prepare_wav_header(opt))
		return NULL;
	/* WAV output over 4 GiB keeps 0xFFFFFFFF sizes (an "oversized" WAV that
	 * most readers play to EOF) unless --rf64 asks for RF64 instead. */
	if (opt->rf64 && !opt->is_wave64 && opt->format != WAV_FMT_AIFF) {
		if (opt->std_out) {
			fprintf(stderr, " Warning: --rf64 needs a seekable output; ignored for stdout.\n");
			opt->rf64 = 0;
		}
		else if (!reserve_ds64(opt)) {
			fprintf(stderr, " Error: unable to reserve the RF64 header.\n");
			return NULL;
		}
	}
	audio_file *aufile = calloc(1, sizeof(audio_file));

	if (!aufile) return NULL;
	aufile->outputFormat = opt->format;

	aufile->samplerate = opt->rate;
	aufile->channels = opt->channels;
	aufile->samples = 0;
	aufile->endianness = opt->endianness;
	aufile->bits_per_sample = opt->samplesize;

	if (opt->std_out) {
		aufile->sndfile = stdout;
#ifdef _WIN32
		_setmode( _fileno(stdout), _O_BINARY );
#endif
	}
	else
		aufile->sndfile = fmkstemp(infile);

	if (aufile->sndfile == NULL) {
		if (aufile)
			free(aufile);
		return NULL;
	}

	{
		int ok;
		if (opt->is_wave64 && aufile->outputFormat != WAV_FMT_AIFF)
			ok = w64_write_header(aufile, opt);
		else if (aufile->outputFormat == WAV_FMT_AIFF) {
			if (opt->std_out) aufile->samples = opt->total_samples_per_channel * opt->channels;
			ok = write_aiff_header(aufile);
			aufile->samples = 0;
		} else {
			Int64_t size = UINT32_MAX;
			if (opt->std_out) {
				Uint64_t bytes = opt->total_samples_per_channel * opt->channels * (opt->samplesize / 8);
				if (bytes > (Uint64_t)INT64_MAX - opt->header_size - 1) ok = 0;
				else {
					size = opt->header_size - 8 + (Int64_t)bytes + (bytes & 1);
					aufile->samples = opt->total_samples_per_channel * opt->channels;
					ok = write_wav_header(aufile, opt, size);
					aufile->samples = 0;
				}
			} else ok = write_wav_header(aufile, opt, size);
		}
		if (!ok || ferror(aufile->sndfile)) {
			if (!opt->std_out) { fclose(aufile->sndfile); remove(infile); }
			free(aufile);
			return NULL;
		}
	}

	return aufile;
}

int write_audio_file(audio_file *aufile, void *sample_buffer, int samples)
{
	switch (aufile->outputFormat) {
		case WAV_FMT_8BIT:
			return write_audio_8bit(aufile, sample_buffer, samples);
		case WAV_FMT_16BIT:
		case WAV_FMT_AIFF:
			return write_audio_16bit(aufile, sample_buffer, samples);
		case WAV_FMT_24BIT:
			return write_audio_24bit(aufile, sample_buffer, samples);
		case WAV_FMT_32BIT:
			return write_audio_32bit(aufile, sample_buffer, samples);
		case WAV_FMT_FLOAT:
			return write_audio_float(aufile, sample_buffer, samples);
		default:
			return 0;
	}

	return 0;
}

/* Copy trailing RIFF metadata with bounded memory and checked I/O. */
static int copy_wav_tail(FILE *in, FILE *out, const wavegain_opt *opt)
{
	unsigned char buffer[65536];
	Int64_t end, pos = opt->data_end;
	/* A newly constructed WAV from AIFF has no RIFF metadata to copy. */
	if (!opt->data_start) return 1;
	if (FSEEK64(in, 0, SEEK_END) || (end = FTELL64(in)) < pos) return 0;
	if ((pos - opt->data_start) & 1) {
		if (pos < end) pos++; /* skip the old data padding, if present */
	}
	if (FSEEK64(in, pos, SEEK_SET)) return 0;
	while (pos < end) {
		size_t count = end - pos > (Int64_t)sizeof(buffer) ? sizeof(buffer) : (size_t)(end - pos);
		if (fread(buffer, 1, count, in) != count || fwrite(buffer, 1, count, out) != count)
			return 0;
		pos += (Int64_t)count;
	}
	return 1;
}

int close_audio_file(FILE *in, audio_file *aufile, wavegain_opt *opt)
{
	Int64_t pos;
	int ok = !ferror(in) && !ferror(aufile->sndfile);
	if (opt->is_wave64 && aufile->outputFormat != WAV_FMT_AIFF) {
		if (!w64_finish(in, aufile, opt)) ok = 0;
	} else if (aufile->outputFormat == WAV_FMT_AIFF) {
		if (!opt->std_out && (FSEEK64(aufile->sndfile, 0, SEEK_SET) ||
		    !write_aiff_header(aufile))) ok = 0;
	} else {
		/* Regenerate padding for the output width, then copy the source tail. */
		if ((aufile->samples * (opt->samplesize / 8)) & 1)
			if (fputc(0, aufile->sndfile) == EOF) ok = 0;
		if (!opt->std_out) {
			if (!copy_wav_tail(in, aufile->sndfile, opt)) ok = 0;
			pos = FTELL64(aufile->sndfile);
			if (pos < 8 || FSEEK64(aufile->sndfile, 0, SEEK_SET) ||
			    !write_wav_header(aufile, opt, pos - 8)) ok = 0;
			else if ((Uint64_t)(pos - 8) > UINT32_MAX) {
				if (opt->rf64) {
					if (!finish_rf64(aufile, opt, pos)) ok = 0;
				} else fprintf(stderr, " Warning: output exceeds 4 GiB; WAV sizes are set to"
				                        " 0xFFFFFFFF (use --rf64 to write RF64).\n");
			}
		}
	}
	if (ferror(aufile->sndfile)) ok = 0;
	if (opt->std_out) {
		if (fflush(aufile->sndfile)) ok = 0;
	} else if (fclose(aufile->sndfile)) ok = 0;
	free(opt->header);
	free(opt);
	free(aufile->scratch);
	free(aufile);
	return ok;
}

#define WRITE_U32(buf, x) *(buf)     = (unsigned char)((x)&0xff);\
                          *((buf)+1) = (unsigned char)(((x)>>8)&0xff);\
                          *((buf)+2) = (unsigned char)(((x)>>16)&0xff);\
                          *((buf)+3) = (unsigned char)(((x)>>24)&0xff);

#define WRITE_U16(buf, x) *(buf)     = (unsigned char)((x)&0xff);\
                          *((buf)+1) = (unsigned char)(((x)>>8)&0xff);

int write_wav_header(audio_file *aufile, wavegain_opt *opt, Int64_t file_size)
{
	size_t fmt, gain;
	unsigned char *p;
	Uint64_t bytes;
	if (file_size < 0 || !wav_header_layout(opt, &fmt, &gain) ||
	    aufile->samples > UINT64_MAX / (opt->samplesize / 8)) return 0;
	bytes = aufile->samples * (opt->samplesize / 8);
	put_le(opt->header + 4, file_size > UINT32_MAX ? UINT32_MAX : (Uint64_t)file_size, 4);
	p = opt->header + fmt + 8;
	put_le(p, aufile->outputFormat == WAV_FMT_FLOAT ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM, 2);
	put_le(p + 2, opt->channels, 2);
	put_le(p + 4, opt->rate, 4);
	put_le(p + 8, (Uint64_t)opt->rate * opt->channels * (opt->samplesize / 8), 4);
	put_le(p + 12, opt->channels * (opt->samplesize / 8), 2);
	put_le(p + 14, opt->samplesize, 2);
	if (gain) WRITE_D64(opt->header + gain + 8, opt->gain_scale);
	put_le(opt->header + opt->header_size - 4, bytes > UINT32_MAX ? UINT32_MAX : bytes, 4);
	return fwrite(opt->header, 1, (size_t)opt->header_size, aufile->sndfile) == (size_t)opt->header_size;
}

/*
 *  Write a 80 bit IEEE854 big endian number as 10 octets. Destination is passed as pointer,
 *  End of destination (p+10) is returned.
 */

static unsigned char* Convert_to_80bit_BE_IEEE854_Float(unsigned char* p, long double val )
{
    unsigned long  word32 = 0x401E;

    if (val > 0.L)
        while (val < (long double)0x80000000)                    // scales value in the range 2^31...2^32
            word32--, val *= 2.L;                              // so you have the exponent

    *p++   = (unsigned char)(word32 >>  8);
    *p++   = (unsigned char)(word32 >>  0);                          // write exponent, sign is assumed as '+'
    word32 = (unsigned long) val;
    *p++   = (unsigned char)(word32 >> 24);
    *p++   = (unsigned char)(word32 >> 16);
    *p++   = (unsigned char)(word32 >>  8);
    *p++   = (unsigned char)(word32 >>  0);                          // write the upper 32 bit of the mantissa
    word32 = (unsigned long) ((val - word32) * 4294967296.L);
    *p++   = (unsigned char)(word32 >> 24);
    *p++   = (unsigned char)(word32 >> 16);
    *p++   = (unsigned char)(word32 >>  8);
    *p++   = (unsigned char)(word32 >>  0);                          // write the lower 32 bit of the mantissa

    return p;
}

int write_aiff_header(audio_file *aufile)
{
	unsigned char   header[54],
	                        *p = header;
	unsigned int         bytes = (aufile->bits_per_sample + 7) / 8;
	unsigned long    data_size = aufile->samples * bytes;
	unsigned long       word32;

	if (!aufile->channels || bytes != 2 || aufile->samples > (UINT32_MAX - 46) / bytes)
		return 0;
	// FORM chunk
	*p++ = 'F';
	*p++ = 'O';
	*p++ = 'R';
	*p++ = 'M';

	word32 = data_size + 0x2E;  // size of the AIFF chunk
	*p++ = (unsigned char)(word32 >> 24);
	*p++ = (unsigned char)(word32 >> 16);
	*p++ = (unsigned char)(word32 >>  8);
	*p++ = (unsigned char)(word32 >>  0);

	*p++ = 'A';
	*p++ = 'I';
	*p++ = 'F';
	*p++ = 'F';
	// end of FORM chunk

	// COMM chunk
	*p++ = 'C';
	*p++ = 'O';
	*p++ = 'M';
	*p++ = 'M';

	word32 = 0x12;                             // size of this chunk
	*p++ = (unsigned char)(word32 >> 24);
	*p++ = (unsigned char)(word32 >> 16);
	*p++ = (unsigned char)(word32 >>  8);
	*p++ = (unsigned char)(word32 >>  0);

	word32 = aufile->channels;                 // channels
	*p++ = (unsigned char)(word32 >>  8);
	*p++ = (unsigned char)(word32 >>  0);

	word32 = aufile->samples / aufile->channels;       // no. of sample frames
	*p++ = (unsigned char)(word32 >> 24);
	*p++ = (unsigned char)(word32 >> 16);
	*p++ = (unsigned char)(word32 >>  8);
	*p++ = (unsigned char)(word32 >>  0);

	word32 = aufile->bits_per_sample;           // bits
	*p++ = (unsigned char)(word32 >>  8);
	*p++ = (unsigned char)(word32 >>  0);

	p = Convert_to_80bit_BE_IEEE854_Float (p, (long double)aufile->samplerate);  // sample frequency as big endian 80 bit IEEE854 float
	// End of COMM chunk

	// SSND chunk
	*p++ = 'S';
	*p++ = 'S';
	*p++ = 'N';
	*p++ = 'D';

	word32 = data_size + 0x08;  // chunk length
	*p++ = (unsigned char)(word32 >> 24);
	*p++ = (unsigned char)(word32 >> 16);
	*p++ = (unsigned char)(word32 >>  8);
	*p++ = (unsigned char)(word32 >>  0);

	*p++ = 0;                                  // offset
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;

	*p++ = 0;                                  // block size
	*p++ = 0;
	*p++ = 0;
	*p++ = 0;

	return fwrite(header, sizeof(header), 1, aufile->sndfile);
}

/* Reuse one packing buffer per output stream. Keep the previous allocation
 * intact if growth fails, and check byte counts before multiplying on ILP32. */
static void *output_buffer(audio_file *out, unsigned int samples, size_t width)
{
    size_t bytes;
    unsigned char *grown;
    if (!width || samples > SIZE_MAX / width) return NULL;
    bytes = (size_t)samples * width;
    if (out->scratch_capacity < bytes) {
        grown = realloc(out->scratch, bytes);
        if (!grown) return NULL;
        out->scratch = grown;
        out->scratch_capacity = bytes;
    }
    return out->scratch;
}

static int write_output_samples(audio_file *out, const void *data,
                                unsigned int samples, size_t width)
{
    if (!samples || !width || samples > SIZE_MAX / width ||
        samples > (Uint64_t)-1 - out->samples) return 0;
    if (fwrite(data, width, samples, out->sndfile) != samples) return 0;
    out->samples += samples;
    return 1;
}

/* Both signed zeros historically serialize as positive zero. */
static uint32_t float_pcm_bits(const float *value)
{
    uint32_t bits;
    memcpy(&bits, value, sizeof(bits));
    return (bits & UINT32_C(0x7fffffff)) ? bits : 0;
}

/* Serialize by bits, never by arithmetic, so subnormals retain their value.
 * This helper also provides the explicit big-endian-host fallback. */
static void pack_float_le(unsigned char *data, const float *input,
                          unsigned int samples)
{
    unsigned int i;
    for (i = 0; i < samples; i++) {
        uint32_t bits = float_pcm_bits(input + i);
        WRITE_U32(data + (size_t)i * 4, bits);
    }
}

int write_audio_8bit(audio_file *aufile, void *sample_buffer, unsigned int samples)
{
	int              ret;
	unsigned int     i;
	unsigned char    *sample_buffer8 = (unsigned char*)sample_buffer;
	unsigned char    *data = output_buffer(aufile, samples, aufile->bits_per_sample / 8);

	if (!data) return 0;

	for (i = 0; i < samples; i++)
		data[i] = (sample_buffer8[i]+128) & 0xFF;

	ret = write_output_samples(aufile, data, samples, aufile->bits_per_sample / 8);


	return ret;
}

int write_audio_16bit(audio_file *aufile, void *sample_buffer, unsigned int samples)
{
	int          ret;
	unsigned int i;
	short        *sample_buffer16 = (short*)sample_buffer;
	char         *data = output_buffer(aufile, samples, aufile->bits_per_sample / 8);

	if (!data) return 0;

#ifdef __APPLE__
		if (aufile->endianness != machine_endianness) {
#else
		if (aufile->endianness == machine_endianness) {
#endif
		for (i = 0; i < samples; i++) {
			data[i*2] = (char)(sample_buffer16[i] & 0xFF);
			data[i*2+1] = (char)((sample_buffer16[i] >> 8) & 0xFF);
		}
	}
	else {
		for (i = 0; i < samples; i++) {
			data[i*2+1] = (char)(sample_buffer16[i] & 0xFF);
			data[i*2] = (char)((sample_buffer16[i] >> 8) & 0xFF);
		}
	}

	ret = write_output_samples(aufile, data, samples, aufile->bits_per_sample / 8);


	return ret;
}

int write_audio_24bit(audio_file *aufile, void *sample_buffer, unsigned int samples)
{
	int          ret;
	unsigned int i;
	int32_t      *sample_buffer24 = (int32_t*)sample_buffer;
	char         *data = output_buffer(aufile, samples, aufile->bits_per_sample / 8);

	if (!data) return 0;

	for (i = 0; i < samples; i++) {
		data[i*3] = (char)(sample_buffer24[i] & 0xFF);
		data[i*3+1] = (char)((sample_buffer24[i] >> 8) & 0xFF);
		data[i*3+2] = (char)((sample_buffer24[i] >> 16) & 0xFF);
	}

	ret = write_output_samples(aufile, data, samples, aufile->bits_per_sample / 8);


	return ret;
}

int write_audio_32bit(audio_file *aufile, void *sample_buffer, unsigned int samples)
{
	int          ret;
	unsigned int i;
	int32_t      *sample_buffer32 = (int32_t*)sample_buffer;
	char         *data = output_buffer(aufile, samples, aufile->bits_per_sample / 8);

	if (!data) return 0;

	for (i = 0; i < samples; i++) {
		data[i*4] = (char)(sample_buffer32[i] & 0xFF);
		data[i*4+1] = (char)((sample_buffer32[i] >> 8) & 0xFF);
		data[i*4+2] = (char)((sample_buffer32[i] >> 16) & 0xFF);
		data[i*4+3] = (char)((sample_buffer32[i] >> 24) & 0xFF);
	}

	ret = write_output_samples(aufile, data, samples, aufile->bits_per_sample / 8);


	return ret;
}

int write_audio_float(audio_file *aufile, void *sample_buffer, unsigned int samples)
{
    const float *input = sample_buffer;
    const float one = 1.f;
    const uint32_t endian = 1;
    uint32_t one_bits;
    unsigned char *data;
    unsigned int i;
    /* WAV float input already requires IEEE binary32; reject an unsupported
     * native layout rather than silently treating it as the wrong format. */
    if (sizeof(float) != sizeof(uint32_t) || FLT_RADIX != 2 ||
        FLT_MANT_DIG != 24 || FLT_MIN_EXP != -125 || FLT_MAX_EXP != 128)
        return 0;
    memcpy(&one_bits, &one, sizeof(one_bits));
    if (one_bits != UINT32_C(0x3f800000)) return 0;
    data = output_buffer(aufile, samples, 4);
    if (!data) return 0;
    if (*(const unsigned char *)&endian) {
        memcpy(data, input, (size_t)samples * 4);
        for (i = 0; i < samples; i++) {
            uint32_t bits;
            memcpy(&bits, data + (size_t)i * 4, sizeof(bits));
            if (!(bits & UINT32_C(0x7fffffff)))
                memset(data + (size_t)i * 4, 0, 4);
        }
    } else {
        pack_float_le(data, input, samples);
    }
    return write_output_samples(aufile, data, samples, 4);
}

void* output_to_PCM(double **input, void *sample_buffer, int channels,
                    int samples, int format)
{
	unsigned char   ch;
	int             i;

	char             *char_sample_buffer = (char*)sample_buffer;
	short           *short_sample_buffer = (short*)sample_buffer;
	int32_t           *int_sample_buffer = (int32_t*)sample_buffer;
	float           *float_sample_buffer = (float*)sample_buffer;

	/*
	 * Copy output to a standard PCM buffer
	 */
	switch (format) {
	case WAV_FMT_8BIT:
		for (ch = 0; ch < channels; ch++) {
			for (i = 0; i < samples; i++) {
				char_sample_buffer[(i*channels)+ch] = (char)input[ch][i];
			}
		}
		break;
	case WAV_FMT_AIFF:
	case WAV_FMT_16BIT:
		for (ch = 0; ch < channels; ch++) {
			for (i = 0; i < samples; i++) {
				short_sample_buffer[(i*channels)+ch] = (short)input[ch][i];
			}
		}
		break;
	case WAV_FMT_24BIT:
	case WAV_FMT_32BIT:
		for (ch = 0; ch < channels; ch++) {
			for (i = 0; i < samples; i++) {
				int_sample_buffer[(i*channels)+ch] = (int32_t)input[ch][i];
			}
		}
		break;
	case WAV_FMT_FLOAT:
		for (ch = 0; ch < channels; ch++) {
			for (i = 0; i < samples; i++) {
				float_sample_buffer[(i*channels)+ch] = (float)input[ch][i];
			}
		}
		break;
	}

	return sample_buffer;
}

/*
 * end of audio.c
 */
