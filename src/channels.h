/* Channel layouts supported by the PCM and BS.1770 adapters. */
#ifndef WG_CHANNELS_H
#define WG_CHANNELS_H

#define WG_MAX_CHANNELS 8
#define WG_SPEAKER_SLOTS 11
#define WG_MASK_51_BACK 0x3fU
#define WG_MASK_51_SIDE 0x60fU
#define WG_MASK_71 0x63fU

static inline int wg_surround_layout(unsigned int channels, unsigned int mask)
{
	return (channels == 6 && (mask == WG_MASK_51_BACK || mask == WG_MASK_51_SIDE)) ||
	       (channels == 8 && mask == WG_MASK_71);
}

/* Preserve the historical mono/left album DC convention for 1/2ch files.
 * Surround files use WAVE speaker bits, never their packed channel index. */
static inline unsigned int wg_channel_slot(unsigned int channels, unsigned int mask,
	unsigned int channel)
{
	unsigned int bit, index = 0;
	if (channels <= 2) return channel;
	for (bit = 0; bit < WG_SPEAKER_SLOTS; bit++) {
		if (!(mask & (1U << bit))) continue;
		if (index++ == channel) return bit;
	}
	return 0; /* Callers validate layouts before using channel indices. */
}

static inline const char *wg_speaker_name(unsigned int slot)
{
	static const char *const names[WG_SPEAKER_SLOTS] = {
		"FL", "FR", "FC", "LFE", "BL", "BR", "FLC", "FRC", "BC", "SL", "SR"
	};
	return names[slot];
}
#endif
