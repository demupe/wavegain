#ifndef MAIN_H
#define MAIN_H

#include "loudness.h"

/* Some version numbers */
#define WAVEGAIN_VERSION "1.5.4"

#define BUFFER_LEN  16384
#define LOG_NAME "WGLog.txt"

#define NO_GAIN -10000.f

#ifdef _WIN32
#include <windows.h>
#endif

/** Information about a file to process */
typedef struct file_list
{
    struct file_list* next_file;
    const char* filename;
    double track_gain;
    double track_peak;
    double dc_offset[WG_MAX_CHANNELS];
    double offset[WG_MAX_CHANNELS];
    double minimum[WG_MAX_CHANNELS], maximum[WG_MAX_CHANNELS];
    double album_dc_offset[WG_MAX_CHANNELS];
    unsigned int channel_mask;
    double raw_gain, duration;
    unsigned long long frames;
    int channels, apply, samplesize, format;
    long rate;
    double loudness, true_peak;
    WG_METER *meter;
    char *output;
    struct seen_input *seen;
} FILE_LIST;


/** Settings and misc other global data */
typedef struct settings
{
    FILE_LIST* file_list;         /**< Files to process (possibly as an album) */
    struct seen_input *seen_inputs; /**< Invocation-wide duplicate input guard */
    struct seen_input **seen_buckets; /**< Path, original-ID and current-ID indexes */
    size_t seen_capacity, seen_count;
#ifdef ENABLE_RECURSIVE
    char* pattern;                /**< Pattern to match file names against */
    int recursive;
#endif
    int first_file;               /**< About to process first file in directory */
    double man_gain;              /**< Apply Manual Gain entered by user */
    double progress_total, progress_done;
    double album_peak;            /**< Will end up storing the highest value of the tracks analyzed */
    int audiophile;               /**< Calculate Album gain */
    int scale;                    /**< write Scale values to stdout */
    int apply_gain;               /**< Apply the calculated gain - album or track */
    int write_chunk;              /**< Write a 'gain' chunk containing the scalefactor applied to the wave data */
    int force;                    /**< Force a file to be re-replaygained when 'gain' chunk present */
    int undo;                     /**< Read the value in the 'gain' chunk and re-scale the data */
    int rf64;                     /**< Write RF64 instead of 0xFFFFFFFF sizes for WAV output over 4 GiB */
    int set_album_gain;           /**< Don't apply the calculated album gain if set */
    int fast;                     /**< Use the fast routines for RG analysis */
    int std_out;                  /**< Write output file to stdout */
    int radio;                    /**< Calculate Title gain  */
    int adc;                      /**< Apply Album based DC Offset correction (default is Track based)  */
    int no_offset;                /**< Do NOT apply any DC Offset  */
    int write_log;                /**< Write a log of gain calculations, etc */
    char *log_data;               /**< Pointer to data to be written to log file */
    int clip_prev;                /**< Whether, or not, to apply clipping prevention */
    int dithering;                /**< Apply dithering to output */
    int shapingtype;              /**< Noise shaping to use in dithering */
    int limiter;                  /**< Apply Hard limiter */
    unsigned int outbitwidth;     /**< bitwidth of desired output */
    unsigned int format;          /**< format of desired output */
    int need_to_process;          /**< need to process even if peak unchanged */
    char* cmd;
    char *output_file, *output_dir;
    const char *output_subdir, *current_output;
    int true_peak, lufs, dither_profile;
    WG_PEAK_MODE true_peak_mode;
    double peak_ceiling, target_lufs;
} SETTINGS;


extern int  add_to_list(FILE_LIST** list, const char* file);
extern void free_list(FILE_LIST* list);
extern int  process_files(FILE_LIST* file_list, SETTINGS* settings, const char* dir);

#endif /* MAIN_H */
