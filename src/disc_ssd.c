#include "disc_ssd.h"

#include "richc/macros.h"
#include "richc/mstr.h"


enum {
    ssd_sector_size      = 256,
    ssd_catalogue_size   = 512,     // sectors 0 and 1
    ssd_first_sector     = 2,       // files pack contiguously from here
    ssd_total_sectors    = 800,     // 80 tracks of 10 sectors: the size the catalogue declares
    ssd_max_files        = 31,
    ssd_max_title        = 12,
    ssd_max_boot         = 3,
    ssd_max_cycle        = 99,
    ssd_address_mask     = 0x3FFFF, // DFS addresses are 18-bit; we truncate and map directly
};

// "<before><name><after>" - the shape of every complaint that names a file.
static rc_str msg(const char *before, rc_str name, const char *after, rc_arena *arena)
{
    rc_mstr s = rc_mstr_make(64, arena);
    rc_mstr_append(&s, rc_str_from_cstr(before), arena);
    rc_mstr_append(&s, name, arena);
    rc_mstr_append(&s, rc_str_from_cstr(after), arena);
    return s.view;
}

// A filename split into DFS terms: a single directory character (default '$') and a 1-7 character name.
// `ok` false means it cannot go on a DFS disc at all.
typedef struct dfs_name {
    char   dir;
    rc_str name;
    bool   ok;
} dfs_name;

// Printable ASCII, minus the characters DFS itself gives meaning to.
static bool dfs_char_ok(char c)
{
    return c > ' ' && c <= '~' && c != '.' && c != ':' && c != '"' && c != '#' && c != '*';
}

static dfs_name dfs_name_parse(rc_str filename)
{
    bool   spec = filename.len >= 2 && filename.data[1] == '.';   // "X.File": a directory specifier
    char   dir  = spec ? filename.data[0] : '$';
    rc_str name = spec ? rc_str_skip(filename, 2) : filename;
    if (!dfs_char_ok(dir) || name.len == 0 || name.len > 7) {
        return (dfs_name) {0};
    }
    for (uint32_t i = 0; i < name.len; i++) {
        if (!dfs_char_ok(name.data[i])) {
            return (dfs_name) {0};
        }
    }
    return (dfs_name) {.dir = dir, .name = name, .ok = true};
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char) (c + 32) : c;
}

// DFS matches names without regard to case, so two names differing only in case still collide.
static bool dfs_name_is_equal(dfs_name a, dfs_name b)
{
    return lower(a.dir) == lower(b.dir) && rc_str_is_equal_insensitive(a.name, b.name);
}

// One placed file: its parsed catalogue name and its start sector. Fixed count (one per entry), so the
// records travel as a span over arena storage.
typedef struct dfs_file {
    dfs_name name;
    uint32_t start;
} dfs_file;

#define RC_ARRAY_TYPE dfs_file
#define RC_ARRAY_NAME dfs_file
#include "richc/template/array.h"

disc_ssd_result disc_ssd_make(const output_spec *spec, rc_arena *arena)
{
    RC_ASSERT(spec != NULL && arena != NULL);
    if (spec->title.len > ssd_max_title) {
        return (disc_ssd_result) {.error = msg("disc title '", spec->title, "' is too long (12 characters maximum)", arena)};
    }
    if (spec->boot > ssd_max_boot) {
        return (disc_ssd_result) {.error = rc_str_from_cstr("boot option must be 0-3")};
    }
    if (spec->cycle > ssd_max_cycle) {
        return (disc_ssd_result) {.error = rc_str_from_cstr("cycle count must be 0-99")};
    }
    uint32_t n = spec->entries.num;
    if (n > ssd_max_files) {
        return (disc_ssd_result) {.error = rc_str_from_cstr("too many files for a DFS disc (31 maximum)")};
    }

    // Parse and validate every name, then place every file: contiguous whole sectors from sector 2, in
    // entry order. Each record remembers where its file landed for the catalogue below.
    rc_span_dfs_file files = rc_span_dfs_file_make(n != 0 ? rc_arena_alloc_type(arena, dfs_file, n) : NULL, n);
    uint32_t next = ssd_first_sector;
    for (uint32_t i = 0; i < n; i++) {
        output_entry e = rc_view_output_entry_get(spec->entries, i);
        dfs_name name = dfs_name_parse(e.filename);
        if (!name.ok) {
            return (disc_ssd_result) {.error = msg("'", e.filename, "' is not a valid DFS filename (1-7 characters plus an optional 'd.' directory prefix)", arena)};
        }
        for (uint32_t j = 0; j < i; j++) {
            if (dfs_name_is_equal(name, rc_span_dfs_file_get(files, j).name)) {
                return (disc_ssd_result) {.error = msg("duplicate DFS filename '", e.filename, "'", arena)};
            }
        }
        rc_span_dfs_file_set(files, i, (dfs_file) {.name = name, .start = next});
        next += (e.code.num + ssd_sector_size - 1) / ssd_sector_size;
        if (next > ssd_total_sectors) {
            return (disc_ssd_result) {.error = msg("disc full: '", e.filename, "' does not fit in 800 sectors", arena)};
        }
    }

    // The image: the catalogue sectors first (all zero until filled in below), then each file's bytes
    // padded to a whole sector. Sized exactly up front, so nothing ever grows.
    rc_array_bytes img = rc_array_bytes_make(next * ssd_sector_size, arena);
    rc_array_bytes_push_n_zero(&img, ssd_catalogue_size, arena);
    for (uint32_t i = 0; i < n; i++) {
        output_entry e = rc_view_output_entry_get(spec->entries, i);
        rc_array_bytes_append(&img, e.code, arena);
        rc_array_bytes_push_n_zero(&img, (ssd_sector_size - e.code.num % ssd_sector_size) % ssd_sector_size, arena);
    }

    // The disc-level catalogue fields: title split 8 + 4 across the two sectors, then the cycle count
    // (BCD), the entry count times 8, and the boot option packed beside the 10-bit sector count.
    for (uint32_t i = 0; i < spec->title.len; i++) {
        rc_array_bytes_set(&img, i < 8 ? i : ssd_sector_size + (i - 8), (uint8_t) spec->title.data[i]);
    }
    rc_array_bytes_set(&img, ssd_sector_size + 4, (uint8_t) (((spec->cycle / 10) << 4) | (spec->cycle % 10)));
    rc_array_bytes_set(&img, ssd_sector_size + 5, (uint8_t) (n * 8));
    rc_array_bytes_set(&img, ssd_sector_size + 6, (uint8_t) (((ssd_total_sectors >> 8) & 3) | (spec->boot << 4)));
    rc_array_bytes_set(&img, ssd_sector_size + 7, (uint8_t) (ssd_total_sectors & 0xFF));

    // The file entries, in DESCENDING start-sector order (the DFS convention: the last file placed is
    // catalogue entry 1) - exactly the reverse of entry order, since files were placed ascending. Sector 0
    // holds the space-padded name plus the directory character (whose top bit would be the locked flag);
    // sector 1 holds load/exec/length low-word little-endian, with each quantity's two high bits packed
    // into the mixed byte alongside the start sector's.
    for (uint32_t slot = 0; slot < n; slot++) {
        uint32_t i = n - 1 - slot;
        output_entry e = rc_view_output_entry_get(spec->entries, i);
        dfs_file f = rc_span_dfs_file_get(files, i);
        uint32_t name_off = 8 + slot * 8;
        for (uint32_t c = 0; c < 7; c++) {
            rc_array_bytes_set(&img, name_off + c, c < f.name.name.len ? (uint8_t) f.name.name.data[c] : ' ');
        }
        rc_array_bytes_set(&img, name_off + 7, (uint8_t) f.name.dir);

        uint32_t load = e.load & ssd_address_mask;
        uint32_t exec = e.exec & ssd_address_mask;
        uint32_t len  = e.code.num;
        uint32_t off  = ssd_sector_size + 8 + slot * 8;
        rc_array_bytes_set(&img, off + 0, (uint8_t) (load & 0xFF));
        rc_array_bytes_set(&img, off + 1, (uint8_t) ((load >> 8) & 0xFF));
        rc_array_bytes_set(&img, off + 2, (uint8_t) (exec & 0xFF));
        rc_array_bytes_set(&img, off + 3, (uint8_t) ((exec >> 8) & 0xFF));
        rc_array_bytes_set(&img, off + 4, (uint8_t) (len & 0xFF));
        rc_array_bytes_set(&img, off + 5, (uint8_t) ((len >> 8) & 0xFF));
        rc_array_bytes_set(&img, off + 6, (uint8_t) (((f.start >> 8) & 3)
                                                   | (((load >> 16) & 3) << 2)
                                                   | (((len >> 16) & 3) << 4)
                                                   | (((exec >> 16) & 3) << 6)));
        rc_array_bytes_set(&img, off + 7, (uint8_t) (f.start & 0xFF));
    }

    return (disc_ssd_result) {.image = img.view};
}

#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(disc_ssd, catalogue_layout_and_truncation)
{
    rc_arena arena = rc_arena_make_default();

    // Two files: a 3-byte one with I/O-processor addresses (&FFFFxxxx truncates to &3xxxx, host bits
    // set), and a 300-byte one under directory X. Placement: boot at sector 2 (1 sector), data at 3 (2).
    static const uint8_t boot_code[] = {0xA9, 0x2A, 0x60};
    uint8_t *table = rc_arena_alloc_zero(&arena, 300);
    table[0]   = 0x11;
    table[299] = 0x99;
    output_entry e[] = {
        {.filename = RC_STR("boot"), .load = 0xFFFF1900, .exec = 0xFFFF1903, .code = RC_VIEW(boot_code)},
        {.filename = RC_STR("X.data"), .load = 0x2000, .exec = 0x2000, .code = rc_view_bytes_make(table, 300)},
    };
    output_spec spec = {.title = RC_STR("Mydisc"), .boot = 3, .cycle = 42, .entries = RC_VIEW(e)};

    disc_ssd_result r = disc_ssd_make(&spec, &arena);
    RC_CHECK(r.error.len, ==, 0u);
    RC_CHECK(r.image.num, ==, 5u * 256u);   // 2 catalogue + 1 + 2 file sectors, truncated there

    // Disc-level fields: title split 8 + 4 (zero-padded), BCD cycle, count * 8, boot beside the 10-bit
    // sector count (800 = &320).
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 0), ==, (uint32_t) 'M');
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 5), ==, (uint32_t) 'c');
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 6), ==, 0u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256), ==, 0u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 4), ==, 0x42u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 5), ==, 16u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 6), ==, 0x33u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 7), ==, 0x20u);

    // Catalogue slot 0 is the LAST file placed (descending start sectors): "data" in directory X at
    // sector 3, load/exec &2000, length 300 (&12C).
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 8), ==, (uint32_t) 'd');
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 8 + 4), ==, (uint32_t) ' ');   // space-padded to 7
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 8 + 7), ==, (uint32_t) 'X');
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 8 + 0), ==, 0x00u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 8 + 1), ==, 0x20u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 8 + 4), ==, 0x2Cu);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 8 + 5), ==, 0x01u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 8 + 6), ==, 0x00u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 8 + 7), ==, 3u);

    // Slot 1 is "boot" in the default directory at sector 2. &FFFF1900 truncates to &31900: low word
    // &1900 little-endian, and the two high bits land in the mixed byte (load at bits 2-3, exec at 6-7).
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 16), ==, (uint32_t) 'b');
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 16 + 7), ==, (uint32_t) '$');
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 16 + 0), ==, 0x00u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 16 + 1), ==, 0x19u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 16 + 2), ==, 0x03u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 16 + 3), ==, 0x19u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 16 + 4), ==, 0x03u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 16 + 5), ==, 0x00u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 16 + 6), ==, 0xCCu);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 16 + 7), ==, 2u);

    // The file bytes themselves: boot at sector 2 (zero-padded to its end), data at sector 3.
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 512), ==, 0xA9u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 514), ==, 0x60u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 515), ==, 0u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 768), ==, 0x11u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 768 + 299), ==, 0x99u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 768 + 300), ==, 0u);

    rc_arena_deinit(&arena);
}

RC_TEST(disc_ssd, empty_disc)
{
    rc_arena arena = rc_arena_make_default();
    output_spec spec = {.title = RC_STR("Empty"), .boot = 0, .cycle = 0};
    disc_ssd_result r = disc_ssd_make(&spec, &arena);
    RC_CHECK(r.error.len, ==, 0u);
    RC_CHECK(r.image.num, ==, 512u);   // just the catalogue
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 5), ==, 0u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 6), ==, 3u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.image, 256 + 7), ==, 0x20u);
    rc_arena_deinit(&arena);
}

RC_TEST(disc_ssd, errors)
{
    rc_arena arena = rc_arena_make_default();
    static const uint8_t byte[] = {0xEA};

    // A name over 7 characters, and one with a dot that is not a directory specifier.
    output_entry longname[] = {{.filename = RC_STR("longname"), .code = RC_VIEW(byte)}};
    output_spec spec = {.entries = RC_VIEW(longname)};
    disc_ssd_result r = disc_ssd_make(&spec, &arena);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("valid DFS filename")));

    output_entry dotted[] = {{.filename = RC_STR("game.bin"), .code = RC_VIEW(byte)}};
    spec.entries = (rc_view_output_entry) RC_VIEW(dotted);
    r = disc_ssd_make(&spec, &arena);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("valid DFS filename")));

    // Names collide without regard to case.
    output_entry dup[] = {
        {.filename = RC_STR("file"), .code = RC_VIEW(byte)},
        {.filename = RC_STR("$.FILE"), .code = RC_VIEW(byte)},
    };
    spec.entries = (rc_view_output_entry) RC_VIEW(dup);
    r = disc_ssd_make(&spec, &arena);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("duplicate DFS filename")));

    // A 32nd file overflows the catalogue (counted before names are even looked at).
    output_entry *many = rc_arena_alloc_type(&arena, output_entry, 32);
    for (uint32_t i = 0; i < 32; i++) {
        many[i] = (output_entry) {.filename = RC_STR("f"), .code = RC_VIEW(byte)};
    }
    spec.entries = rc_view_output_entry_make(many, 32);
    r = disc_ssd_make(&spec, &arena);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("31 maximum")));

    // One file bigger than the 798 data sectors of an 80-track disc.
    uint32_t huge_len = 799 * 256 + 1;
    output_entry huge[] = {{.filename = RC_STR("huge"), .code = rc_view_bytes_make(rc_arena_alloc_zero(&arena, huge_len), huge_len)}};
    spec.entries = (rc_view_output_entry) RC_VIEW(huge);
    r = disc_ssd_make(&spec, &arena);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("disc full")));

    // Disc-level limits: a 13-character title, boot option 4, cycle 100.
    spec = (output_spec) {.title = RC_STR("ThirteenChars")};
    r = disc_ssd_make(&spec, &arena);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("too long")));
    spec = (output_spec) {.boot = 4};
    r = disc_ssd_make(&spec, &arena);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("boot option")));
    spec = (output_spec) {.cycle = 100};
    r = disc_ssd_make(&spec, &arena);
    RC_CHECK_TRUE(rc_str_contains(r.error, RC_STR("cycle count")));

    rc_arena_deinit(&arena);
}

#endif // BARON_TESTS
