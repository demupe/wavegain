/* Preserve run-wide duplicate protection while bounding hash lookup work. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t path_comparisons;
static int fail_allocation;
static int counted_strcmp(const char *a, const char *b)
{
	path_comparisons++; return strcmp(a, b);
}
static void *index_calloc(size_t count, size_t size)
{
	return fail_allocation ? NULL : calloc(count, size);
}
#define strcmp counted_strcmp
#define calloc index_calloc
#define main wavegain_main
#include "../src/main.c"
#undef main
#undef calloc
#undef strcmp

static INPUT_IDENTITY identity(unsigned int value)
{
	INPUT_IDENTITY id = {0}; id.valid = 1;
#ifdef _WIN32
	id.volume = 12; id.high = 34; id.low = value;
#else
	id.device = 12; id.inode = value;
#endif
	return id;
}

static void insert(SETTINGS *settings, const char *path, INPUT_IDENTITY original, INPUT_IDENTITY current)
{
	struct seen_input *seen = calloc(1, sizeof(*seen));
	assert(seen && reserve_seen_inputs(settings));
	seen->path = strdup(path); assert(seen->path);
	seen->original = original; seen->current = current;
	seen->next = settings->seen_inputs; settings->seen_inputs = seen;
	index_seen_input(settings, seen); settings->seen_count++;
}

int main(void)
{
	SETTINGS settings = {0};
	INPUT_IDENTITY invalid = {0}, old = identity(1), current = identity(2), other = identity(3);
	char path[64];
	insert(&settings, "original-name", old, current);
	assert(input_seen(&settings, "original-name", &other));
	assert(input_seen(&settings, "hard-link-to-original", &old));
	assert(input_seen(&settings, "hard-link-to-replacement", &current));
	assert(!input_seen(&settings, "unseen", &other));
	insert(&settings, "missing-identity", invalid, invalid);
	assert(input_seen(&settings, "missing-identity", &other));
	assert(input_seen(&settings, "missing-identity", &invalid));
	/* Grow across many rehashes, covering collisions and invalid-ID fallback. */
	for (unsigned int i = 0; i < 20000; i++) {
		INPUT_IDENTITY id = identity(i + 10);
		snprintf(path, sizeof(path), "unique-path-%u", i);
		insert(&settings, path, id, id);
	}
	path_comparisons = 0;
	for (unsigned int i = 0; i < 20000; i++) {
		INPUT_IDENTITY id = identity(i + 10);
		snprintf(path, sizeof(path), "unique-path-%u", i);
		assert(input_seen(&settings, path, &invalid));
		assert(input_seen(&settings, "unseen", &id));
	}
	assert(path_comparisons < 100000);
	assert(input_seen(&settings, "unseen", &old) && input_seen(&settings, "unseen", &current));
	/* A failed resize must leave the previous indexes usable. */
	{
		size_t count = settings.seen_count;
		settings.seen_count = settings.seen_capacity;
		fail_allocation = 1; assert(!reserve_seen_inputs(&settings)); fail_allocation = 0;
		settings.seen_count = count;
		assert(input_seen(&settings, "original-name", &invalid));
		assert(input_seen(&settings, "unseen", &current));
	}
	/* Refresh removes the previous current key and preserves the original. */
	{
		FILE_LIST file = {0};
		INPUT_IDENTITY actual;
		insert(&settings, "refresh-name", identity(40000), identity(50000));
		file.filename = "test/floating_point_exception.wav";
		file.seen = settings.seen_inputs;
		old = file.seen->original; current = file.seen->current;
		input_identity(file.filename, &actual); assert(actual.valid);
		refresh_seen_input(&settings, &file);
		assert(input_seen(&settings, "unseen", &old));
		assert(input_seen(&settings, "unseen", &actual));
		assert(!input_seen(&settings, "unseen", &current));
		/* Refresh once more with a distinct current ID and a failed stat. */
		file.filename = "test/absent-input-index-fixture.wav";
		refresh_seen_input(&settings, &file);
		assert(!input_seen(&settings, "unseen", &actual));
		assert(input_seen(&settings, "unseen", &old));
	}
	free_seen_inputs(&settings);
	assert(!settings.seen_inputs && !settings.seen_buckets && !settings.seen_count && !settings.seen_capacity);
	puts("Input index: path/ID aliases, rehashes, bounded lookups, allocation failure and refresh passed");
	return 0;
}
