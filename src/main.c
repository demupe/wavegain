/*
 * function: ReplayGain support for Wave files (http://www.replaygain.org)
 *
 * Reads in a Vorbis file, figures out the peak and ReplayGain levels and
 * applies the Gain tags to the Wave file
 *
 * This program is distributed under the GNU General Public License, version 
 * 2.1. A copy of this license is included with this source.
 *
 * Copyright (C) 2002-2004 John Edwards
 * Additional code by Magnus Holmgren and Gian-Carlo Pascutto
 * Linux patch by Marc Brooker
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include "getopt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include "output.h"
#include "config.h"
#include "gain_analysis.h"
#include "i18n.h"
#include "audio.h"
#include "misc.h"
#include "wavegain.h"
#include "main.h"
#include "dither.h"

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef ENABLE_RECURSIVE
#include "recurse.h"
#endif

/*This is an ugly way of doing this - but it works for the moment*/
#ifndef MAX_PATH
#define MAX_PATH 512
#endif

int	      write_to_log = 0;
char          log_file_name[MAX_PATH];
extern double total_samples;

/**
 * \brief Allocate a FILE_LIST node.
 *
 * Allocate a FILE_LIST node. filename is set to file while track_peak and
 * track_gain are set to NO_PEAK and NO_GAIN respectively. The rest of the
 * fields are set to zero.
 *
 * \param file  name of file to get a node for.
 * \return  the allocated node or NULL (in which case a message has been
 *          printed).
 */
static FILE_LIST* alloc_node(const char* file)
{
	FILE_LIST* node;

	node = calloc(1, sizeof(*node));

	if (node != NULL) {
		node->filename = strdup(file);

		if (node->filename != NULL) {
			node->track_peak = NO_PEAK;
			node->track_gain = NO_GAIN;
			node->dc_offset[0] = 0.;
			node->dc_offset[1] = 0.;
			node->offset[0] = 0.;
			node->offset[1] = 0.;
			return node;
		}
		free(node);
	}

	fprintf(stderr, _("Out of memory\n"));
	return NULL;
}


/**
 * \brief Allocate a new FILE_LIST node and add it to the end of a list.
 *
 * Allocate a new FILE_LIST node and add it to the end of a list.
 *
 * \param list  pointer to the list (first node) pointer. If the list
 *              pointer contains NULL, it is set to the new node. Otherwise 
 *              the new node is added the last node in the list.
 * \param file  name of the file to get a node for.
 * \return  0 if successful and -1 if the node couldn't be allocated (in
 *          which case a message has been printed).
 */
int add_to_list(FILE_LIST** list, const char* file)
{
	if (*list == NULL) {
		*list = alloc_node(file);

		if (*list == NULL)
			return -1;
	}
	else {
		FILE_LIST* node = *list;

		while (node->next_file != NULL)
			node = node->next_file;

		node->next_file = alloc_node(file);

		if (node->next_file == NULL)
			return -1;
	}

	return 0;
}


/**
 * Free all nodes in a FILE_LIST list.
 *
 * \param list  pointer to first node in list. Can be NULL, in which case 
 *              nothing is done.
 */
void free_list(FILE_LIST* list)
{
	FILE_LIST* next;

	while (list != NULL) {
		next = list->next_file;
		free((void *) list->filename);
		free(list->output);
		wg_meter_free(list->meter);
		free(list);
		list = next;
	}
}

static char s_floatToAscii[256];
const char* ftos(double f, const char* format)
{
	sprintf(s_floatToAscii, format, f);
	return s_floatToAscii;
}

/**
 * \brief Processs the file in file_list.
 *
 * Process the files in file_list. If settings->album is set, the files are
 * considered to belong to one album.
 *
 * \param file_list  list of files to process.
 * \param settings   settings and global variables.
 * \return  0 if successful and -1 if an error occured (in which case a
 *          message has been printed).
 */
static int prepare_outputs(FILE_LIST *list, SETTINGS *settings)
{
	FILE_LIST *file, *other;
	if (!settings->output_file && !settings->output_dir) return 1;
	if (settings->output_file && list && list->next_file) {
		fprintf(stderr, " --output requires exactly one input file.\n"); return 0;
	}
	for (file = list; file; file = file->next_file) {
		struct stat st;
		if (settings->output_file) file->output = strdup(settings->output_file);
		else {
			char *folder = settings->output_subdir && *settings->output_subdir ?
				wg_output_join(settings->output_dir, settings->output_subdir) : strdup(settings->output_dir);
			if (!folder || !wg_output_mkdirs(folder)) { free(folder); return 0; }
			file->output = wg_output_join(folder, last_path(file->filename));
			free(folder);
		}
		if (!file->output) return 0;
		if (!stat(file->output, &st)) {
			fprintf(stderr, " Refusing to overwrite output %s.\n", file->output); return 0;
		}
		if (errno != ENOENT) { file_error(" Cannot inspect output %s: ", file->output); return 0; }
		for (other = list; other != file; other = other->next_file) {
#ifdef _WIN32
			if (!_stricmp(other->output, file->output))
#else
			if (!strcmp(other->output, file->output))
#endif
			{ fprintf(stderr, " Duplicate output %s.\n", file->output); return 0; }
		}
	}
	return 1;
}

typedef struct input_identity {
	int valid;
#ifdef _WIN32
	DWORD volume, high, low;
#else
	dev_t device;
	ino_t inode;
#endif
} INPUT_IDENTITY;

struct seen_input {
	struct seen_input *next;
	struct seen_input *path_next, *original_next, *current_next;
	char *path;
	INPUT_IDENTITY original, current;
};

static void input_identity(const char *path, INPUT_IDENTITY *id)
{
	memset(id, 0, sizeof(*id));
#ifdef _WIN32
	BY_HANDLE_FILE_INFORMATION info;
	HANDLE handle = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
	                            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) return;
	if (GetFileInformationByHandle(handle, &info)) {
		id->valid = 1;
		id->volume = info.dwVolumeSerialNumber;
		id->high = info.nFileIndexHigh; id->low = info.nFileIndexLow;
	}
	CloseHandle(handle);
#else
	struct stat st;
	if (!stat(path, &st)) { id->valid = 1; id->device = st.st_dev; id->inode = st.st_ino; }
#endif
}

static int same_input(const INPUT_IDENTITY *a, const INPUT_IDENTITY *b)
{
	if (!a->valid || !b->valid) return 0;
#ifdef _WIN32
	return a->volume == b->volume && a->high == b->high && a->low == b->low;
#else
	return a->device == b->device && a->inode == b->inode;
#endif
}

static uint64_t input_hash_mix(uint64_t value)
{
	value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
	return value ^ (value >> 31);
}

static size_t input_path_bucket(const char *path, size_t capacity)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	while (*path) { hash ^= (unsigned char)*path++; hash *= UINT64_C(1099511628211); }
	return (size_t)hash & (capacity - 1);
}

static size_t input_id_bucket(const INPUT_IDENTITY *id, size_t capacity)
{
#ifdef _WIN32
	uint64_t first = id->volume, second = ((uint64_t)id->high << 32) | id->low;
#else
	uint64_t first = (uint64_t)id->device, second = (uint64_t)id->inode;
#endif
	return (size_t)(input_hash_mix(first) ^ input_hash_mix(second + UINT64_C(0x9e3779b97f4a7c15))) & (capacity - 1);
}

static void index_seen_input(SETTINGS *settings, struct seen_input *seen)
{
	size_t capacity = settings->seen_capacity;
	struct seen_input **bucket = settings->seen_buckets + input_path_bucket(seen->path, capacity);
	seen->path_next = *bucket; *bucket = seen;
	if (seen->original.valid) {
		bucket = settings->seen_buckets + capacity + input_id_bucket(&seen->original, capacity);
		seen->original_next = *bucket; *bucket = seen;
	}
	if (seen->current.valid) {
		bucket = settings->seen_buckets + 2 * capacity + input_id_bucket(&seen->current, capacity);
		seen->current_next = *bucket; *bucket = seen;
	}
}

static int reserve_seen_inputs(SETTINGS *settings)
{
	size_t capacity = settings->seen_capacity;
	struct seen_input **buckets, *seen;
	if (capacity && settings->seen_count < capacity - capacity / 4) return 1;
	if (capacity > SIZE_MAX / 2) return 0;
	capacity = capacity ? capacity * 2 : 64;
	if (capacity > SIZE_MAX / 3 / sizeof(*buckets)) return 0;
	buckets = calloc(3 * capacity, sizeof(*buckets));
	if (!buckets) return 0;
	free(settings->seen_buckets);
	settings->seen_buckets = buckets; settings->seen_capacity = capacity;
	for (seen = settings->seen_inputs; seen; seen = seen->next) index_seen_input(settings, seen);
	return 1;
}

static int input_seen(const SETTINGS *settings, const char *path, const INPUT_IDENTITY *id)
{
	size_t capacity = settings->seen_capacity;
	struct seen_input *seen;
	/* Always check paths: an earlier pathname can now refer to a different ID. */
	for (seen = settings->seen_buckets[input_path_bucket(path, capacity)]; seen; seen = seen->path_next)
		if (!strcmp(path, seen->path)) return 1;
	if (!id->valid) return 0;
	for (seen = settings->seen_buckets[capacity + input_id_bucket(id, capacity)]; seen; seen = seen->original_next)
		if (same_input(id, &seen->original)) return 1;
	for (seen = settings->seen_buckets[2 * capacity + input_id_bucket(id, capacity)]; seen; seen = seen->current_next)
		if (same_input(id, &seen->current)) return 1;
	return 0;
}

/* Atomic output publication changes the ID. Remove the old current key while
 * retaining the original key to recognize aliases in later directory batches. */
static void refresh_seen_input(SETTINGS *settings, FILE_LIST *file)
{
	struct seen_input *seen = file->seen, **bucket;
	size_t capacity = settings->seen_capacity;
	if (seen->current.valid) {
		bucket = settings->seen_buckets + 2 * capacity + input_id_bucket(&seen->current, capacity);
		while (*bucket != seen) bucket = &(*bucket)->current_next;
		*bucket = seen->current_next;
	}
	input_identity(file->filename, &seen->current);
	seen->current_next = NULL;
	if (seen->current.valid) {
		bucket = settings->seen_buckets + 2 * capacity + input_id_bucket(&seen->current, capacity);
		seen->current_next = *bucket; *bucket = seen;
	}
}

static void free_seen_inputs(SETTINGS *settings)
{
	while (settings->seen_inputs) {
		struct seen_input *seen = settings->seen_inputs;
		settings->seen_inputs = seen->next;
		free(seen->path); free(seen);
	}
	free(settings->seen_buckets); settings->seen_buckets = NULL;
	settings->seen_capacity = settings->seen_count = 0;
}

/* Retain both identities: replacing one name can break a hard/symbolic link.
 * A later batch must recognize the old alias and the newly written name. */
static int remember_inputs(FILE_LIST *files, SETTINGS *settings)
{
	FILE_LIST *file;
	for (file = files; file; file = file->next_file) {
		INPUT_IDENTITY id;
		struct seen_input *seen;
		char *path;
		if (!file->filename) continue;
#ifdef _WIN32
		path = _fullpath(NULL, file->filename, 0);
#else
		path = realpath(file->filename, NULL);
		if (!path) {
			char *cwd = getcwd(NULL, 0);
			path = file->filename[0] == '/' ? strdup(file->filename) :
				cwd ? wg_output_join(cwd, file->filename) : NULL;
			free(cwd);
		}
#endif
		if (!path) return 0;
		input_identity(file->filename, &id);
		if (!reserve_seen_inputs(settings)) { free(path); return 0; }
		if (input_seen(settings, path, &id)) {
			fprintf(stderr, " Duplicate input refused: %s.\n", file->filename);
			free(path); return 0;
		}
		seen = calloc(1, sizeof(*seen));
		if (!seen) { free(path); return 0; }
		seen->path = path; seen->original = seen->current = id;
		seen->next = settings->seen_inputs;
		settings->seen_inputs = file->seen = seen;
		index_seen_input(settings, seen); settings->seen_count++;
	}
	return 1;
}

#ifdef _WIN32
/* Variables published to --exec commands; keep in sync with process_files()
 * and doc/help.json. */
static const char *const exec_variables[] = {
	"INPUT_FILE", "INPUT_FDIR", "INPUT_RDIR", "INPUT_NAME",
	"TRACK_GAIN", "TRACK_PEAK", "TRACK_SCALE", "TRACK_NEW_PEAK",
	"ALBUM_GAIN", "ALBUM_PEAK", "ALBUM_SCALE", "ALBUM_NEW_PEAK",
	"DC_OFFSET_L", "DC_OFFSET_R"
};

/* cmd.exe expands %VAR% before parsing, so file names containing & | < > ^ ( )
 * would become command syntax. Delayed !VAR! expansion runs after parsing and
 * is never re-parsed, so switch WaveGain's own variables to it. A literal '!'
 * cannot be represented reliably under /v:on and is rejected. */
static int delay_exec_variables(char *command)
{
	char *p;
	size_t i, n;
	for (p = command; *p; p++) {
		if (*p == '!') return 0;
		if (*p != '%') continue;
		for (i = 0; i < sizeof(exec_variables) / sizeof(*exec_variables); i++) {
			n = strlen(exec_variables[i]);
			if (!_strnicmp(p + 1, exec_variables[i], n) && p[n + 1] == '%') {
				p[0] = p[n + 1] = '!';
				p += n + 1;
				break;
			}
		}
	}
	return 1;
}

/* Run a command prepared by build_command() through the system cmd.exe:
 * /d skips AutoRun, /v:on enables the delayed variables, and /s strips only
 * the enclosing quotes added here. */
static int run_exec_command(const char *command)
{
	static const char options[] = " /d /v:on /s /c \"";
	char shell[MAX_PATH + 9], *line;
	UINT length = GetSystemDirectoryA(shell, MAX_PATH);
	STARTUPINFOA startup;
	PROCESS_INFORMATION process;
	BOOL started;
	if (!length || length >= MAX_PATH) return 0;
	strcpy(shell + length, "\\cmd.exe");
	line = malloc(strlen(shell) + sizeof(options) + strlen(command) + 3);
	if (!line) return 0;
	sprintf(line, "\"%s\"%s%s\"", shell, options, command);
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	fflush(stdout);
	fflush(stderr);
	started = CreateProcessA(shell, line, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
	free(line);
	if (!started) return 0;
	WaitForSingleObject(process.hProcess, INFINITE);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return 1;
}
#endif

int process_files(FILE_LIST* file_list, SETTINGS* settings, const char* dir WG_UNUSED)
{
	FILE_LIST* file;
	int failed = 0;
	double     audiophile_gain = 0.,
	           scale = 1.,
	           album_dc_offset[WG_SPEAKER_SLOTS] = {0.},
	           album_channel_seconds[WG_SPEAKER_SLOTS] = {0.},
	           album_dc_compensation[WG_SPEAKER_SLOTS] = {0.};

	settings->album_peak = NO_PEAK;

	if (file_list == NULL)
		return 0;

	if (!remember_inputs(file_list, settings) || !prepare_outputs(file_list, settings)) return -1;
	settings->first_file = 1;
	total_samples = 0.;

	/* Undo previously applied gain */
    settings->progress_total = settings->progress_done = 0.;
    if (settings->undo) {
        for (file = file_list; file; file = file->next_file)
            if (file->filename) settings->progress_total += audio_duration(file->filename, settings);
        for (file = file_list; file; file = file->next_file) {
			if (file->filename == NULL)
				continue;
			settings->current_output = file->output;
			if (!write_gains(file->filename, 0, 0, 0, 0, 0, settings)) {
				fprintf(stderr, " Error processing GAIN for file - %s\n", file->filename);
				failed = 1;
				continue;
			}
			refresh_seen_input(settings, file);
		}
	}
	else {
		/* Analyze the files */
		for (file = file_list; file; file = file->next_file) {
			if (file->filename == NULL)
				continue;

			{
				int status = get_gain(file->filename, &file->track_peak, &file->track_gain,
				                      file->dc_offset, file->offset, settings, file);
				if (status != 1) {
					if (!status) failed = 1; /* A deliberate skip returns 2. */
					free((void *)file->filename);
					file->filename = NULL;
					continue;
				}
			}

			{
				int ch;
				for (ch = 0; ch < file->channels; ch++) {
					unsigned int slot = wg_channel_slot(file->channels, file->channel_mask, ch);
					volatile double part = file->offset[ch] / file->rate - album_dc_compensation[slot];
					volatile double next = album_dc_offset[slot] + part;
					album_dc_compensation[slot] = (next - album_dc_offset[slot]) - part;
					album_dc_offset[slot] = next;
					album_channel_seconds[slot] += file->duration;
				}
			}
		}

        if (settings->first_file) return failed ? -1 : 0;
        /* Missing speaker positions contribute no duration to their DC mean. */
        {
            unsigned int slot;
            for (slot = 0; slot < WG_SPEAKER_SLOTS; slot++)
                if (album_channel_seconds[slot] > 0.)
                    album_dc_offset[slot] /= album_channel_seconds[slot];
            for (file = file_list; file; file = file->next_file) {
                int ch;
                if (!file->filename) continue;
                for (ch = 0; ch < file->channels; ch++)
                    file->album_dc_offset[ch] = album_dc_offset[
                        wg_channel_slot(file->channels, file->channel_mask, ch)];
            }
        }
        settings->album_peak = 0.;
        for (file = file_list; file; file = file->next_file) {
            const double *dc;
            if (!file->filename) continue;
            dc = settings->adc ? file->album_dc_offset : file->dc_offset;
            if ((settings->lufs || settings->true_peak) && !measure_file(file, dc, settings)) {
                /* Do not publish an album whose measurements are incomplete. */
                return -1;
            }
            report_gain(file, dc, settings);
            settings->album_peak = fmax(settings->album_peak, corrected_peak(file, dc, settings));
        }

		if (!settings->no_offset && settings->adc) {
			double dc_l = trunc(album_dc_offset[0] * -32768.) + 0.;
			double dc_r = trunc(album_dc_offset[1] * -32768.) + 0.;
			fprintf(stderr, " ********************* Album DC Offset | %4.0f  |  %4.0f  | ***************\n",
				dc_l, dc_r);
			if(write_to_log) {
				write_log(" ********************* Album DC Offset | %4.0f  |  %4.0f  | ***************\n",
					dc_l, dc_r);
			}
		}

		fprintf(stderr, "\n");
		if(write_to_log)
			write_log("\n");

        if (settings->audiophile) {
            double raw_album = GetAlbumGain();
            if (settings->lufs) {
                size_t count = 0, i = 0;
                double loudness;
                WG_METER **meters;
                for (file = file_list; file; file = file->next_file) if (file->filename) count++;
                if (!count || count > (size_t)-1 / sizeof(*meters)) return -1;
                meters = malloc(count * sizeof(*meters));
                if (!meters) return -1;
                for (file = file_list; file; file = file->next_file) if (file->filename) meters[i++] = file->meter;
                if (!wg_meter_album(meters, count, &loudness)) { free(meters); return -1; }
                free(meters);
                raw_album = isfinite(loudness) ? settings->target_lufs - loudness : 0.;
                fprintf(stderr, " Album integrated loudness: %.2f LUFS\n", loudness);
            }
            audiophile_gain = selected_gain(raw_album, settings->album_peak, settings);
            if (settings->true_peak) for (file = file_list; file; file = file->next_file)
                if (file->filename) audiophile_gain = fmin(audiophile_gain, true_peak_gain(file, settings));
            scale = pow(10., audiophile_gain * .05);
            if (settings->scale) fprintf(stdout, "%8.6lf", scale);
            fprintf(stderr, " Recommended Album Gain: %+6.2f dB\tScale: %6.4f\n\n", audiophile_gain, scale);
            if (write_to_log) write_log(" Recommended Album Gain: %+6.2f dB\tScale: %6.4f\n\n", audiophile_gain, scale);
        }

        if (settings->apply_gain) {
            settings->progress_total = settings->progress_done = 0.;
            for (file = file_list; file; file = file->next_file) {
                const double *dc;
                double gain;
                int correction;
                if (!file->filename) continue;
                dc = settings->adc ? file->album_dc_offset : file->dc_offset;
                gain = settings->audiophile ? audiophile_gain : file->track_gain;
                {
                    int ch;
                    correction = 0;
                    for (ch = 0; !settings->no_offset && ch < file->channels; ch++)
                        if (dc[ch] != 0.) correction = 1;
                }
                file->apply = file->output || settings->true_peak || settings->lufs || settings->need_to_process || settings->force || settings->limiter ||
                              gain != 0. || correction;
                if (file->apply) settings->progress_total += file->duration;
            }
            for (file = file_list; file; file = file->next_file) {
                if (!file->filename) continue;
                if (!file->apply) {
                    fprintf(stderr, " No Gain adjustment or DC correction required for file: %s, skipping.\n", file->filename);
                    continue;
                }
                settings->current_output = file->output;
                if (!write_gains(file->filename, file->track_gain, audiophile_gain, file->track_peak,
                                 file->dc_offset, file->album_dc_offset, settings)) {
                    fprintf(stderr, " Error processing GAIN for file - %s\n", file->filename);
                    failed = 1;
                }
				refresh_seen_input(settings, file);
            }
        }
	}

	fprintf(stderr, "\n WaveGain Processing completed %s\n", failed ? "with errors" : "normally");
	if (write_to_log)
		write_log("\n WaveGain Processing completed %s\n\n", failed ? "with errors" : "normally");

#ifdef _WIN32
	if (settings->cmd) { 	/* execute user command */
		FILE_LIST* file;
		char *buff;
		char *b, *p, *q;
		double track_scale = 1.0;

		SetEnvironmentVariable("ALBUM_GAIN", ftos(audiophile_gain, "%.2lf"));
		SetEnvironmentVariable("ALBUM_SCALE", ftos(scale, "%.5lf"));
		SetEnvironmentVariable("ALBUM_NEW_PEAK", ftos(settings->album_peak * scale, "%.0lf"));
		SetEnvironmentVariable("ALBUM_PEAK", ftos(settings->album_peak, "%.0lf"));

		for (file = file_list; file; file = file->next_file) {
			if (file->filename == NULL)
				continue;

			if (dir[0] == '.' && dir[1] == '\\') dir += 2;

			buff = strdup(file->filename);
			if (!buff) { failed = 1; break; }
			b = (buff[0] == '.' && buff[1] == '\\') ? buff+2 : buff;
			SetEnvironmentVariable("INPUT_FILE", b);

			p = strrchr(b, '\\');
			if (p) {
				p[0] = '\0'; ++p;
				SetEnvironmentVariable("INPUT_FDIR", b);
				SetEnvironmentVariable("INPUT_RDIR", b); // assume dir = "."
			}
			else {
				p = b;
				SetEnvironmentVariable("INPUT_FDIR", ".");
				SetEnvironmentVariable("INPUT_RDIR", dir);
			}

			q = strrchr(p, '.');
			if (q) q[0] = '\0';
			SetEnvironmentVariable("INPUT_NAME", p);

			track_scale = (pow(10., file->track_gain * 0.05));
			SetEnvironmentVariable("TRACK_SCALE", ftos(track_scale, "%.5lf"));
			SetEnvironmentVariable("TRACK_GAIN", ftos(file->track_gain, "%.2lf"));
			SetEnvironmentVariable("TRACK_NEW_PEAK", ftos(file->track_peak, "%.0lf"));
			SetEnvironmentVariable("TRACK_PEAK", ftos(file->track_peak / track_scale, "%.0lf"));

			SetEnvironmentVariable("DC_OFFSET_L", ftos(trunc(file->dc_offset[0] * -32768.) + 0., "%.0lf"));
			SetEnvironmentVariable("DC_OFFSET_R", ftos(trunc(file->dc_offset[1] * -32768.) + 0., "%.0lf"));

			fprintf(stderr, "\n Executing command on \"%s\":\n\n", file->filename);
			if (!run_exec_command(settings->cmd)) {
				fprintf(stderr, " Error: cannot start the command.\n");
				failed = 1;
			}
			free(buff);
		}
	}
#endif

	return failed ? -1 : 0;
}


/**
 * Print out a list of options and the command line syntax.
 */
static void usage(void)
{
    fprintf(stdout, _("WaveGain v" WAVEGAIN_VERSION " Compiled " __DATE__ ".\n"));
#include "help_generated.h"
}


enum { OPT_OUTPUT = 256, OPT_OUTPUT_DIR, OPT_TRUE_PEAK, OPT_PEAK_CEILING,
       OPT_LUFS, OPT_TARGET_LUFS, OPT_DITHER_PROFILE, OPT_TRUE_PEAK_MODE };

static int parse_level(const char *text, double *value, double minimum, double maximum)
{
	char *end;
	errno = 0;
	*value = strtod(text, &end);
	return !errno && end != text && !*end && isfinite(*value) && *value >= minimum && *value <= maximum;
}

static const struct option long_options[] = {
	{"output", 1, NULL, OPT_OUTPUT},
	{"output-dir", 1, NULL, OPT_OUTPUT_DIR},
	{"true-peak", 0, NULL, OPT_TRUE_PEAK},
	{"true-peak-mode", 1, NULL, OPT_TRUE_PEAK_MODE},
	{"peak-ceiling", 1, NULL, OPT_PEAK_CEILING},
	{"lufs", 0, NULL, OPT_LUFS},
	{"target-lufs", 1, NULL, OPT_TARGET_LUFS},
	{"dither-profile", 1, NULL, OPT_DITHER_PROFILE},
	{"help",	0, NULL, 'h'},
	{"album",	0, NULL, 'a'},
	{"radio",	0, NULL, 'r'},
	{"adc",		0, NULL, 'q'},
	{"no_offset",	0, NULL, 'p'},
	{"calculate",	0, NULL, 'c'},
	{"scale",	0, NULL, 'x'},
	{"apply",	0, NULL, 'y'},
	{"write",	0, NULL, 'w'},
	{"force",	0, NULL,  0 },
	{"undo-gain",	0, NULL,  0 },
	{"rf64",	0, NULL,  0 },
	{"fast",	0, NULL, 's'},
	{"stdout",	0, NULL, 'o'},
#ifdef ENABLE_RECURSIVE
	{"recursive",   0, NULL, 'z'},
#endif
	{"log",		0, NULL, 'l'},
	{"logfile",	1, NULL, 'f'},
	{"noclip",	0, NULL, 'n'},
	{"dither",	1, NULL, 'd'},
	{"limiter",	0, NULL, 't'},
	{"gain",	1, NULL, 'g'},
	{"bits",	1, NULL, 'b'},
#ifdef _WIN32
	{"exec",	1, NULL, 'e'},
#endif
	{NULL,		0, NULL , 0}
};


#ifdef ENABLE_RECURSIVE
#define ARG_STRING "harqpcxywsozlf:nd:tg:b:e:"
#else
#define ARG_STRING "harqpcxywsolf:nd:tg:b:e:"
#endif


#ifdef _WIN32
/* system() uses cmd.exe; reject oversized commands before constructing them. */
static char *build_command(const char *program, int argc, char **argv)
{
	const size_t limit = 8191;
	const char *name = program;
	char *full = NULL, *short_name = NULL, *command = NULL, *p;
	DWORD needed, written;
	size_t length, name_length;
	int quote, i;
	if (strlen(program) > limit) return NULL;
	needed = GetFullPathNameA(program, 0, NULL, NULL);
	if (needed) {
		if (needed > 32768 || !(full = malloc(needed))) goto done;
		written = GetFullPathNameA(program, needed, full, NULL);
		if (!written || written >= needed) goto done;
		needed = GetShortPathNameA(full, NULL, 0);
		if (needed) {
			if (needed > 32768 || !(short_name = malloc(needed))) goto done;
			written = GetShortPathNameA(full, short_name, needed);
			if (!written || written >= needed) goto done;
			name = short_name;
		}
	}
	name_length = strlen(name);
	/* Preserve shell fragments such as "echo hello". Quote actual executable
	 * paths only; run_exec_command() adds the enclosing pair for cmd /s. */
	quote = strpbrk(name, " \t") && (name == short_name ||
		(full && GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES));
	if (name_length > limit - 2 * quote) goto done;
	length = name_length + 2 * quote;
	for (i = 0; i < argc; i++) {
		size_t argument = strlen(argv[i]);
		if (length > limit - 3 || argument > limit - length - 3) goto done;
		length += argument + 3;
	}
	command = malloc(length + 1);
	if (!command) goto done;
	p = command;
	if (quote) *p++ = '"';
	memcpy(p, name, name_length); p += name_length;
	if (quote) *p++ = '"';
	for (i = 0; i < argc; i++) {
		size_t argument = strlen(argv[i]);
		*p++ = ' '; *p++ = '"';
		memcpy(p, argv[i], argument); p += argument;
		*p++ = '"';
	}
	*p = 0;
	if (!delay_exec_variables(command)) {
		free(command);
		command = NULL;
	}
done:
	free(full);
	free(short_name);
	return command;
}
#endif

int main(int argc, char** argv)
{
	SETTINGS settings;
	int      option_index = 1,
	         ret,
	         i,
	         bits;

#ifdef _WIN32
 	char     CmdDir[MAX_PATH];
 	char     *p;
#endif

	memset(&settings, 0, sizeof(settings));
	settings.first_file = 1;
	settings.radio = 1;
	settings.clip_prev = 1;
	settings.outbitwidth = 16;
	settings.format = WAV_NO_FMT;
	settings.peak_ceiling = -1.;
	settings.target_lufs = -23.;

#ifdef _WIN32
	/* Is this good enough? Or do we need to consider multi-byte codepages as 
	* well? 
	*/
	SetConsoleOutputCP(GetACP());

	GetModuleFileName(NULL, CmdDir, MAX_PATH);
	p = strrchr(CmdDir, '\\') + 1;
	p[0] = '\0';
#endif

	while ((ret = getopt_long(argc, argv, ARG_STRING, long_options, &option_index)) != -1) {
		switch(ret) {
			case OPT_OUTPUT: settings.output_file = optarg; break;
			case OPT_OUTPUT_DIR: settings.output_dir = optarg; break;
			case OPT_TRUE_PEAK: settings.true_peak = 1; break;
			case OPT_TRUE_PEAK_MODE:
				if (!strcmp(optarg, "precise")) settings.true_peak_mode = WG_PEAK_PRECISE;
				else if (!strcmp(optarg, "fast")) settings.true_peak_mode = WG_PEAK_FAST;
				else if (!strcmp(optarg, "extended")) settings.true_peak_mode = WG_PEAK_EXTENDED;
				else { fprintf(stderr, " --true-peak-mode requires precise, fast or extended.\n"); return EXIT_FAILURE; }
				settings.true_peak = 1; break;
			case OPT_LUFS: settings.lufs = 1; break;
			case OPT_PEAK_CEILING:
				if (!parse_level(optarg, &settings.peak_ceiling, -60., 0.)) {
					fprintf(stderr, " --peak-ceiling requires a finite value from -60 to 0 dBTP.\n"); return EXIT_FAILURE;
				}
				settings.true_peak = 1; break;
			case OPT_TARGET_LUFS:
				if (!parse_level(optarg, &settings.target_lufs, -70., -5.)) {
					fprintf(stderr, " --target-lufs requires a finite value from -70 to -5 LUFS.\n"); return EXIT_FAILURE;
				}
				settings.lufs = 1; break;
			case OPT_DITHER_PROFILE:
				if (!strcmp(optarg, "rate")) settings.dither_profile = 1;
				else if (!strcmp(optarg, "legacy")) settings.dither_profile = 0;
				else { fprintf(stderr, " Unknown dither profile: %s.\n", optarg); return EXIT_FAILURE; }
				break;
			case 0:
				if (!strcmp(long_options[option_index].name, "force")) {
					settings.force = 1;
				}
				else if (!strcmp(long_options[option_index].name, "undo-gain")) {
					settings.undo = 1;
				}
				else if (!strcmp(long_options[option_index].name, "rf64")) {
					settings.rf64 = 1;
				}
				else {
					fprintf(stderr, "Internal error parsing command line options\n");
					exit(1);
				}
				break;
			case 'h':
				usage();
				exit(0);
				break;
			case 'a':
				settings.audiophile = 1;
				break;
			case 'r':
				settings.radio = 1;
				break;
			case 'q':
				settings.adc = 1;
				break;
			case 'p':
				settings.no_offset = 1;
				break;
			case 'c':
				settings.apply_gain = 0;
				break;
			case 'x':
				settings.scale = 1;
				break;
			case 'y':
				settings.apply_gain = 1;
				settings.scale = 0;
				break;
			case 'w':
				settings.write_chunk = 1;
				break;
			case 's':
				settings.fast = 1;
				break;
			case 'o':
				settings.std_out = 1;
				break;
#ifdef ENABLE_RECURSIVE
			case 'z':
				settings.recursive = 1;
				break;
#endif
			case 'l':
				write_to_log = 1;
#ifdef _WIN32
				if (strlen(CmdDir) + strlen(LOG_NAME) >= sizeof(log_file_name)) {
					fprintf(stderr, "Log filename is too long.\n");
					return EXIT_FAILURE;
				}
				strcpy(log_file_name, CmdDir);
				strcat(log_file_name, LOG_NAME);
#else
				strcpy(log_file_name, LOG_NAME);
#endif
				break;
			case 'f':
				write_to_log = 1;
				if (strlen(optarg) >= sizeof(log_file_name)) {
					fprintf(stderr, "Log filename is too long.\n");
					return EXIT_FAILURE;
				}
				strcpy(log_file_name, optarg);
				break;
			case 'n':
				settings.clip_prev = 0;
				break;
			case 'd':
				settings.need_to_process = 1;
				settings.dithering = 1;
   				if(sscanf(optarg, "%d", &settings.shapingtype) != 1) {
	    				fprintf(stderr, "Warning: dither type %s not recognised, using default\n", optarg);
		    			break;
				}
				if (settings.shapingtype == 0)
					settings.dithering = 0;
				else if (settings.shapingtype == 1)
					settings.shapingtype = 0;
				else if (settings.shapingtype == 2)
					settings.shapingtype = 1;
				else if (settings.shapingtype == 3)
					settings.shapingtype = 2;
				else if (settings.shapingtype == 4)
					settings.shapingtype = 3;
				break;
			case 't':
				settings.limiter = 1;
				break;
			case '?':
				return EXIT_FAILURE;
			case 'g':
				if(sscanf(optarg, "%lf", &settings.man_gain) != 1 || !isfinite(settings.man_gain)) {
	    				fprintf(stderr, "Warning: manual gain %s not recognised, ignoring\n", optarg);
					settings.man_gain = 0.;
					break;
				}
				if(settings.man_gain < -20.0) {
	    				fprintf(stderr, "Warning: manual gain %s is out of range, "
					                 "applying gain of -20.0dB\n", optarg);
					settings.man_gain = -20.0;
				}
				else if(settings.man_gain > 12.0) {
	    				fprintf(stderr, "Warning: manual gain %s is out of range, "
					                 "applying gain of +12.0dB\n", optarg);
					settings.man_gain = 12.0;
				}
				break;
			case 'b':
				settings.need_to_process = 1;
   				if(sscanf(optarg, "%d", &bits) != 1) {
	    				fprintf(stderr, "Warning: output format %s not recognised, using default\n", optarg);
		    			break;
				}
				if (bits == 1) {
					settings.outbitwidth = 8;
					settings.format = WAV_FMT_8BIT;
		    			break;
				}
				else if (bits == 2) {
					settings.outbitwidth = 16;
					settings.format = WAV_FMT_16BIT;
		    			break;
				}
				else if (bits == 3) {
					settings.outbitwidth = 24;
					settings.format = WAV_FMT_24BIT;
		    			break;
				}
				else if (bits == 4) {
					settings.outbitwidth = 32;
					settings.format = WAV_FMT_32BIT;
		    			break;
				}
				else if (bits == 5) {
					settings.outbitwidth = 32;
					settings.format = WAV_FMT_FLOAT;
		    			break;
				}
				else if (bits == 6) {
					settings.outbitwidth = 16;
					settings.format = WAV_FMT_AIFF;
		    			break;
				}
				else {
	    				fprintf(stderr, "Warning: output format %s not recognised, using default\n", optarg);
					break;
				}
				break;
#ifdef _WIN32
			case 'e': {
				free(settings.cmd);
				settings.cmd = build_command(optarg, argc - optind, argv + optind);
				if (settings.cmd == NULL) {
					fprintf(stderr, "Command exceeds the shell limit, contains '!', or cannot be prepared.\n");
					return 1;
				}
				argc = optind;
				break;
			}
#endif
		}
	}
	if ((settings.fast || settings.undo) && (settings.lufs || settings.true_peak)) {
		fprintf(stderr, " LUFS/true-peak measurement cannot be combined with fast/undo mode.\n"); return EXIT_FAILURE;
	}
	if (settings.dither_profile && (!settings.dithering || settings.format == WAV_FMT_FLOAT)) {
		fprintf(stderr, " Rate dither requires -d 1..4 and integer output.\n"); return EXIT_FAILURE;
	}
	if (settings.output_file && settings.output_dir) {
		fprintf(stderr, " Choose --output or --output-dir.\n"); return EXIT_FAILURE;
	}
	if (settings.output_file || settings.output_dir) {
		char *absolute;
		if (settings.std_out || settings.fast || (!settings.apply_gain && !settings.undo) ||
		    optind >= argc || !strcmp(argv[optind], "-")) {
			fprintf(stderr, " Output paths require file input and -y or --undo-gain, without --stdout/--fast.\n"); return EXIT_FAILURE;
		}
		if (settings.output_file) {
			struct stat st;
			if (argc - optind != 1 || strpbrk(argv[optind], "*?") ||
			    stat(argv[optind], &st) || (st.st_mode & S_IFMT) == S_IFDIR) {
				fprintf(stderr, " --output requires exactly one explicit input file.\n"); return EXIT_FAILURE;
			}
		}
		absolute = wg_output_absolute(settings.output_file ? settings.output_file : settings.output_dir, !settings.output_file);
		if (!absolute) { fprintf(stderr, " Cannot prepare output path.\n"); return EXIT_FAILURE; }
		if (settings.output_file) settings.output_file = absolute;
		else {
			settings.output_dir = absolute;
			for (i = optind; i < argc; i++) {
				struct stat st;
				if (!stat(argv[i], &st) && (st.st_mode & S_IFMT) == S_IFDIR) {
					int inside = wg_output_inside(argv[i], absolute);
					if (inside) {
						fprintf(stderr, inside < 0 ? " Cannot verify output directory location.\n" :
						        " Output directory must be outside input directories.\n");
						free(absolute); return EXIT_FAILURE;
					}
				}
			}
		}
	}
	if (optind < argc && !strcmp(argv[optind], "-") && (settings.lufs || settings.true_peak) && !settings.no_offset) {
		fprintf(stderr, " LUFS/true-peak on stdin requires -p; DC-corrected analysis needs seekable input.\n"); return EXIT_FAILURE;
	}

	if (settings.fast) {
		settings.apply_gain = 0;
		settings.no_offset = 1;
	}
	if (settings.undo == 1) {
		settings.write_chunk = 0;
		settings.no_offset = 1;
	}

	if (optind >= argc) {
		fprintf(stderr, _("No files specified.\n"));
		usage();
		return EXIT_SUCCESS;
	}

	if (write_to_log) {
		write_log("Command line:\n\t");
		for(i = 0; i < argc; i++)
			write_log("%s ",  argv[i]);
		write_log("\n");
	}

	if (settings.true_peak) {
		const char *mode = settings.true_peak_mode == WG_PEAK_FAST ? "fast" :
			settings.true_peak_mode == WG_PEAK_EXTENDED ? "extended" : "precise";
		fprintf(stderr, " True-peak mode: %s\n", mode);
		if (write_to_log) write_log(" True-peak mode: %s\n", mode);
	}
	if (!strcmp(argv[optind], "-")) {
		FILE_LIST info = {0};
		int status;
		info.filename = "-";
		status = get_gain("-", &info.track_peak, &info.track_gain,
		                  info.dc_offset, info.offset, &settings, &info);
		if (status != 1) { wg_meter_free(info.meter); return status == 2 ? EXIT_SUCCESS : EXIT_FAILURE; }
		if ((settings.lufs || settings.true_peak) && !measure_file(&info, info.dc_offset, &settings)) {
			wg_meter_free(info.meter); return EXIT_FAILURE;
		}
		report_gain(&info, info.dc_offset, &settings);
		wg_meter_free(info.meter);
		return EXIT_SUCCESS;
	}
	else {
		for (i = optind; i < argc; ++i) {
#ifdef ENABLE_RECURSIVE
			if (process_argument(argv[i], &settings) < 0) {
				free_list(settings.file_list);
				free_seen_inputs(&settings);
				return EXIT_FAILURE;
			}
#else
			if (add_to_list(&settings.file_list, argv[i]) < 0)
				return EXIT_FAILURE;
#endif
		}

		/* Process files (perhaps remaining) in list */
		ret = process_files(settings.file_list, &settings, ".");
		free_list(settings.file_list);
		settings.file_list = NULL;
		if (settings.cmd) free(settings.cmd);
		settings.cmd = NULL;
	}

	free(settings.output_file);
	free(settings.output_dir);
	free_seen_inputs(&settings);
	return (ret < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
