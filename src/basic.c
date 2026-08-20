#include "basic.h"

#include "richc/macros.h"


// The keyword flag bits, exactly as the ROM's table carries them. They are consumed in a fixed order
// after a match: conditional first (it can veto the match entirely), pseudo_var before the token is
// written, then middle / start / fnproc / line_number / rest_raw once it has been.
enum {
    basic_flag_conditional = 0x01,   // not a keyword after all if a variable character follows (COUNTER)
    basic_flag_middle      = 0x02,   // we are now mid-statement (and no line number is expected)
    basic_flag_start       = 0x04,   // we are back at a statement start (THEN, ELSE, LET, ERROR)
    basic_flag_fnproc      = 0x08,   // FN/PROC: the name that follows is the user's, copied untouched
    basic_flag_line_number = 0x10,   // a bare number from here on is a line number (GOTO and friends)
    basic_flag_rest_raw    = 0x20,   // REM/DATA: the rest of the line is not ours to interpret
    basic_flag_pseudo_var  = 0x40,   // statement form gets token + &40 (PAGE=8 vs PRINT PAGE)
};

typedef struct basic_keyword {
    rc_str  text;
    uint8_t token;
    uint8_t flags;
} basic_keyword;

// The BASIC 4 keyword table, in the ROM's exact order (&8456) - the order IS the abbreviation
// precedence (AND before ABS makes "A." mean AND), and first letters never decrease, which is what
// lets the matcher abandon the scan early. COLOR arrived in BASIC 3 and EDIT in BASIC 4; otherwise
// this is the table every BBC BASIC shares. WIDTH's token &FE doubles as the ROM's end-of-table
// sentinel, so it must stay last. TOP, TIME$ and the '#' file forms are deliberately absent - the
// ROM composes them (TO + 'P', TIME + '$', token + '#') and so do we, for free.
static const basic_keyword basic_keywords[] = {
    {RC_STR_INIT("AND"),      0x80, 0x00},
    {RC_STR_INIT("ABS"),      0x94, 0x00},
    {RC_STR_INIT("ACS"),      0x95, 0x00},
    {RC_STR_INIT("ADVAL"),    0x96, 0x00},
    {RC_STR_INIT("ASC"),      0x97, 0x00},
    {RC_STR_INIT("ASN"),      0x98, 0x00},
    {RC_STR_INIT("ATN"),      0x99, 0x00},
    {RC_STR_INIT("AUTO"),     0xC6, 0x10},
    {RC_STR_INIT("BGET"),     0x9A, 0x01},
    {RC_STR_INIT("BPUT"),     0xD5, 0x03},
    {RC_STR_INIT("COLOUR"),   0xFB, 0x02},
    {RC_STR_INIT("CALL"),     0xD6, 0x02},
    {RC_STR_INIT("CHAIN"),    0xD7, 0x02},
    {RC_STR_INIT("CHR$"),     0xBD, 0x00},
    {RC_STR_INIT("CLEAR"),    0xD8, 0x01},
    {RC_STR_INIT("CLOSE"),    0xD9, 0x03},
    {RC_STR_INIT("CLG"),      0xDA, 0x01},
    {RC_STR_INIT("CLS"),      0xDB, 0x01},
    {RC_STR_INIT("COS"),      0x9B, 0x00},
    {RC_STR_INIT("COUNT"),    0x9C, 0x01},
    {RC_STR_INIT("COLOR"),    0xFB, 0x02},
    {RC_STR_INIT("DATA"),     0xDC, 0x20},
    {RC_STR_INIT("DEG"),      0x9D, 0x00},
    {RC_STR_INIT("DEF"),      0xDD, 0x00},
    {RC_STR_INIT("DELETE"),   0xC7, 0x10},
    {RC_STR_INIT("DIV"),      0x81, 0x00},
    {RC_STR_INIT("DIM"),      0xDE, 0x02},
    {RC_STR_INIT("DRAW"),     0xDF, 0x02},
    {RC_STR_INIT("ENDPROC"),  0xE1, 0x01},
    {RC_STR_INIT("END"),      0xE0, 0x01},
    {RC_STR_INIT("ENVELOPE"), 0xE2, 0x02},
    {RC_STR_INIT("ELSE"),     0x8B, 0x14},
    {RC_STR_INIT("EVAL"),     0xA0, 0x00},
    {RC_STR_INIT("ERL"),      0x9E, 0x01},
    {RC_STR_INIT("ERROR"),    0x85, 0x04},
    {RC_STR_INIT("EOF"),      0xC5, 0x01},
    {RC_STR_INIT("EOR"),      0x82, 0x00},
    {RC_STR_INIT("ERR"),      0x9F, 0x01},
    {RC_STR_INIT("EXP"),      0xA1, 0x00},
    {RC_STR_INIT("EXT"),      0xA2, 0x01},
    {RC_STR_INIT("EDIT"),     0xCE, 0x10},
    {RC_STR_INIT("FOR"),      0xE3, 0x02},
    {RC_STR_INIT("FALSE"),    0xA3, 0x01},
    {RC_STR_INIT("FN"),       0xA4, 0x08},
    {RC_STR_INIT("GOTO"),     0xE5, 0x12},
    {RC_STR_INIT("GET$"),     0xBE, 0x00},
    {RC_STR_INIT("GET"),      0xA5, 0x00},
    {RC_STR_INIT("GOSUB"),    0xE4, 0x12},
    {RC_STR_INIT("GCOL"),     0xE6, 0x02},
    {RC_STR_INIT("HIMEM"),    0x93, 0x43},
    {RC_STR_INIT("INPUT"),    0xE8, 0x02},
    {RC_STR_INIT("IF"),       0xE7, 0x02},
    {RC_STR_INIT("INKEY$"),   0xBF, 0x00},
    {RC_STR_INIT("INKEY"),    0xA6, 0x00},
    {RC_STR_INIT("INT"),      0xA8, 0x00},
    {RC_STR_INIT("INSTR("),   0xA7, 0x00},
    {RC_STR_INIT("LIST"),     0xC9, 0x10},
    {RC_STR_INIT("LINE"),     0x86, 0x00},
    {RC_STR_INIT("LOAD"),     0xC8, 0x02},
    {RC_STR_INIT("LOMEM"),    0x92, 0x43},
    {RC_STR_INIT("LOCAL"),    0xEA, 0x02},
    {RC_STR_INIT("LEFT$("),   0xC0, 0x00},
    {RC_STR_INIT("LEN"),      0xA9, 0x00},
    {RC_STR_INIT("LET"),      0xE9, 0x04},
    {RC_STR_INIT("LOG"),      0xAB, 0x00},
    {RC_STR_INIT("LN"),       0xAA, 0x00},
    {RC_STR_INIT("MID$("),    0xC1, 0x00},
    {RC_STR_INIT("MODE"),     0xEB, 0x02},
    {RC_STR_INIT("MOD"),      0x83, 0x00},
    {RC_STR_INIT("MOVE"),     0xEC, 0x02},
    {RC_STR_INIT("NEXT"),     0xED, 0x02},
    {RC_STR_INIT("NEW"),      0xCA, 0x01},
    {RC_STR_INIT("NOT"),      0xAC, 0x00},
    {RC_STR_INIT("OLD"),      0xCB, 0x01},
    {RC_STR_INIT("ON"),       0xEE, 0x02},
    {RC_STR_INIT("OFF"),      0x87, 0x00},
    {RC_STR_INIT("OR"),       0x84, 0x00},
    {RC_STR_INIT("OPENIN"),   0x8E, 0x00},
    {RC_STR_INIT("OPENOUT"),  0xAE, 0x00},
    {RC_STR_INIT("OPENUP"),   0xAD, 0x00},
    {RC_STR_INIT("OSCLI"),    0xFF, 0x02},
    {RC_STR_INIT("PRINT"),    0xF1, 0x02},
    {RC_STR_INIT("PAGE"),     0x90, 0x43},
    {RC_STR_INIT("PTR"),      0x8F, 0x43},
    {RC_STR_INIT("PI"),       0xAF, 0x01},
    {RC_STR_INIT("PLOT"),     0xF0, 0x02},
    {RC_STR_INIT("POINT("),   0xB0, 0x00},
    {RC_STR_INIT("PROC"),     0xF2, 0x0A},
    {RC_STR_INIT("POS"),      0xB1, 0x01},
    {RC_STR_INIT("RETURN"),   0xF8, 0x01},
    {RC_STR_INIT("REPEAT"),   0xF5, 0x00},
    {RC_STR_INIT("REPORT"),   0xF6, 0x01},
    {RC_STR_INIT("READ"),     0xF3, 0x02},
    {RC_STR_INIT("REM"),      0xF4, 0x20},
    {RC_STR_INIT("RUN"),      0xF9, 0x01},
    {RC_STR_INIT("RAD"),      0xB2, 0x00},
    {RC_STR_INIT("RESTORE"),  0xF7, 0x12},
    {RC_STR_INIT("RIGHT$("),  0xC2, 0x00},
    {RC_STR_INIT("RND"),      0xB3, 0x01},
    {RC_STR_INIT("RENUMBER"), 0xCC, 0x10},
    {RC_STR_INIT("STEP"),     0x88, 0x00},
    {RC_STR_INIT("SAVE"),     0xCD, 0x02},
    {RC_STR_INIT("SGN"),      0xB4, 0x00},
    {RC_STR_INIT("SIN"),      0xB5, 0x00},
    {RC_STR_INIT("SQR"),      0xB6, 0x00},
    {RC_STR_INIT("SPC"),      0x89, 0x00},
    {RC_STR_INIT("STR$"),     0xC3, 0x00},
    {RC_STR_INIT("STRING$("), 0xC4, 0x00},
    {RC_STR_INIT("SOUND"),    0xD4, 0x02},
    {RC_STR_INIT("STOP"),     0xFA, 0x01},
    {RC_STR_INIT("TAN"),      0xB7, 0x00},
    {RC_STR_INIT("THEN"),     0x8C, 0x14},
    {RC_STR_INIT("TO"),       0xB8, 0x00},
    {RC_STR_INIT("TAB("),     0x8A, 0x00},
    {RC_STR_INIT("TRACE"),    0xFC, 0x12},
    {RC_STR_INIT("TIME"),     0x91, 0x43},
    {RC_STR_INIT("TRUE"),     0xB9, 0x01},
    {RC_STR_INIT("UNTIL"),    0xFD, 0x02},
    {RC_STR_INIT("USR"),      0xBA, 0x00},
    {RC_STR_INIT("VDU"),      0xEF, 0x02},
    {RC_STR_INIT("VAL"),      0xBB, 0x00},
    {RC_STR_INIT("VPOS"),     0xBC, 0x01},
    {RC_STR_INIT("WIDTH"),    0xFE, 0x02},   // token &FE is the end-of-table sentinel: keep WIDTH last
};

enum {
    basic_max_line_number = 32767,   // the ROM's 16-bit accumulate refuses anything larger
    basic_max_record      = 255,     // the length byte counts the whole record, itself included
    basic_end_of_table    = 0xFE,    // WIDTH's token: the scan stops here, never past it
};


// ---- character predicates (the ROM's, not the assembler's) ----

static bool basic_is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

// Valid in a BASIC variable name (the ROM's classifier at &8D84): letters of either case, digits,
// '_' and '`'. Note '`' really is in - it displays as the pound sign on the Beeb.
static bool basic_is_var_char(uint8_t c)
{
    if (c >= 0x7B) return false;
    if (c >= 0x5F) return true;    // '_', '`', 'a'-'z'
    if (c >= 0x5B) return false;   // '[' '\' ']' '^'
    if (c >= 0x41) return true;    // 'A'-'Z'
    if (c >= 0x3A) return false;   // ':' ';' '<' '=' '>' '?' '@'
    return c >= 0x30;              // '0'-'9'
}

// A digit or UPPERCASE hex letter: the run a '&' literal consumes. Lowercase ends the run, just as
// it does in the ROM (a-f sort above 'G' and fail its range check).
static bool basic_is_hex_char(uint8_t c)
{
    return basic_is_digit(c) || (c >= 'A' && c <= 'F');
}

static uint8_t basic_at(rc_str text, uint32_t pos)
{
    RC_ASSERT(pos < text.len);
    return (uint8_t) text.data[pos];
}


// ---- emission helpers ----

// The &8D embedded line-number form: three bytes whose top bits are shuffled into the first so that
// all three land in &40-&7F - safely above '"' and below every token, so the interpreter's scans
// never trip over one mid-line.
static void basic_push_line_number(rc_array_bytes *out, uint32_t n, rc_arena *arena)
{
    rc_array_bytes_push(out, 0x8D, arena);
    rc_array_bytes_push(out, (uint8_t) (((((n & 0xC000) >> 12) | ((n & 0x00C0) >> 2))) ^ 0x54), arena);
    rc_array_bytes_push(out, (uint8_t) ((n & 0x3F) | 0x40), arena);
    rc_array_bytes_push(out, (uint8_t) (((n >> 8) & 0x3F) | 0x40), arena);
}

// Copy [pos, end) through untouched.
static void basic_push_raw(rc_array_bytes *out, rc_str text, uint32_t pos, uint32_t end, rc_arena *arena)
{
    for (uint32_t i = pos; i < end; i++) {
        rc_array_bytes_push(out, basic_at(text, i), arena);
    }
}


// ---- the keyword matcher ----

typedef struct basic_match {
    uint32_t entry;   // index into basic_keywords, or RC_INDEX_NONE
    uint32_t next;    // offset just past the matched text (the '.' included, when abbreviated)
} basic_match;

// Try the table against text at pos (whose first character is 'A'-'W'). The winner is the FIRST
// entry, in table order, that either matches in full or matches up to a '.' in the source (the
// abbreviation rule: "P." is PRINT because PRINT comes first). First letters in the table never
// decrease, so a source letter below the current entry's can never match anything later - the scan
// gives up there, and otherwise stops at the &FE sentinel.
static basic_match basic_find_keyword(rc_str text, uint32_t pos)
{
    uint8_t c = basic_at(text, pos);
    for (uint32_t k = 0; ; k++) {
        const basic_keyword *e = &basic_keywords[k];
        uint8_t first = (uint8_t) e->text.data[0];
        if (c < first) {
            return (basic_match) {.entry = RC_INDEX_NONE};
        }
        if (c == first) {
            uint32_t i = 1;
            while (i < e->text.len && pos + i < text.len && basic_at(text, pos + i) == (uint8_t) e->text.data[i]) {
                i++;
            }
            if (i == e->text.len) {
                return (basic_match) {.entry = k, .next = pos + i};
            }
            if (pos + i < text.len && basic_at(text, pos + i) == '.') {
                return (basic_match) {.entry = k, .next = pos + i + 1};
            }
        }
        if (e->token == basic_end_of_table) {
            return (basic_match) {.entry = RC_INDEX_NONE};
        }
    }
}


// ---- the tokeniser proper ----

// Tokenise `text` (the line with its number already peeled off) into `out`, following the ROM's
// dispatch at &8DB2 case for case. Two state flags drive everything: `sos` ("start of statement" -
// cleared by nearly anything, set again by ':' and a few keywords) decides '*' commands and the
// pseudo-variable +&40 rule; `lnexp` ("a line number would tokenise here") starts TRUE - the ROM
// leaves it set after the leading line number, which is why 10 GOTO10 and even a bare 10 20 embed
// &8D forms - and survives spaces, commas, '&' literals and strings, exactly as the ROM's does.
static void basic_tokenise_text(rc_array_bytes *out, rc_str text, rc_arena *arena)
{
    bool sos = true;
    bool lnexp = true;
    uint32_t pos = 0;
    while (pos < text.len) {
        uint8_t c = basic_at(text, pos);

        if (c == ' ') {
            rc_array_bytes_push(out, c, arena);
            pos++;
            continue;                                       // spaces change nothing, not even lnexp
        }
        if (c == '&') {
            rc_array_bytes_push(out, c, arena);
            pos++;
            while (pos < text.len && basic_is_hex_char(basic_at(text, pos))) {
                rc_array_bytes_push(out, basic_at(text, pos), arena);
                pos++;
            }
            continue;                                       // a hex literal changes no state either
        }
        if (c == '"') {
            rc_array_bytes_push(out, c, arena);
            pos++;
            while (pos < text.len && basic_at(text, pos) != '"') {
                rc_array_bytes_push(out, basic_at(text, pos), arena);
                pos++;
            }
            if (pos == text.len) {
                return;                                     // unterminated: the rest went through raw
            }
            rc_array_bytes_push(out, '"', arena);
            pos++;
            continue;
        }
        if (c == ':') {
            rc_array_bytes_push(out, c, arena);
            pos++;
            sos = true;
            lnexp = false;
            continue;
        }
        if (c == ',') {
            rc_array_bytes_push(out, c, arena);
            pos++;
            continue;                                       // ',' keeps lnexp: LIST 10,20 embeds both
        }
        if (c == '*') {
            if (sos) {
                basic_push_raw(out, text, pos, text.len, arena);   // a * command: the OS gets it verbatim
                return;
            }
            rc_array_bytes_push(out, c, arena);
            pos++;
            sos = false;
            lnexp = false;
            continue;
        }
        if (basic_is_digit(c) && lnexp) {
            uint32_t end = pos;
            uint32_t n = 0;
            while (end < text.len && basic_is_digit(basic_at(text, end))) {
                if (n <= basic_max_line_number) {           // saturate: only "too big" matters
                    n = n * 10 + (uint32_t) (basic_at(text, end) - '0');
                }
                end++;
            }
            if (n <= basic_max_line_number) {
                basic_push_line_number(out, n, arena);
                pos = end;
                continue;                                   // lnexp deliberately stays set
            }
            // Too big for a line number: fall through and let it be an ordinary numeric literal.
        }
        if (c == '.' || basic_is_digit(c)) {
            while (pos < text.len && (basic_at(text, pos) == '.' || basic_is_digit(basic_at(text, pos)))) {
                rc_array_bytes_push(out, basic_at(text, pos), arena);
                pos++;
            }
            sos = false;
            lnexp = false;
            continue;
        }
        if (c >= 'A' && c < 'X') {                          // only 'A'-'W' can start a keyword
            basic_match m = basic_find_keyword(text, pos);
            if (m.entry != RC_INDEX_NONE) {
                const basic_keyword *e = &basic_keywords[m.entry];
                bool rejected = (e->flags & basic_flag_conditional) &&
                                m.next < text.len && basic_is_var_char(basic_at(text, m.next));
                if (!rejected) {
                    uint8_t token = e->token;
                    if ((e->flags & basic_flag_pseudo_var) && sos) {
                        token += 0x40;                      // the statement form: PAGE=&E00, TIME=0, ...
                    }
                    rc_array_bytes_push(out, token, arena);
                    pos = m.next;
                    if (e->flags & basic_flag_middle) {
                        sos = false;
                        lnexp = false;
                    }
                    if (e->flags & basic_flag_start) {
                        sos = true;
                        lnexp = false;
                    }
                    if (e->flags & basic_flag_fnproc) {     // the FN/PROC name belongs to the user
                        while (pos < text.len && basic_is_var_char(basic_at(text, pos))) {
                            rc_array_bytes_push(out, basic_at(text, pos), arena);
                            pos++;
                        }
                    }
                    if (e->flags & basic_flag_line_number) {
                        lnexp = true;                       // applied last, so ELSE/THEN keep it despite bit 2
                    }
                    if (e->flags & basic_flag_rest_raw) {   // REM and DATA own the rest of the line
                        basic_push_raw(out, text, pos, text.len, arena);
                        return;
                    }
                    continue;
                }
            }
            // No keyword (or a conditional one vetoed): it is a variable name - fall through.
        }
        if (basic_is_var_char(c)) {
            while (pos < text.len && basic_is_var_char(basic_at(text, pos))) {
                rc_array_bytes_push(out, basic_at(text, pos), arena);
                pos++;
            }
        }
        else {
            rc_array_bytes_push(out, c, arena);             // any other punctuation passes through
            pos++;
        }
        sos = false;
        lnexp = false;
    }
}

basic_line_result basic_tokenise_line(rc_str line, rc_arena *arena)
{
    RC_ASSERT(line.len > 0 && basic_is_digit((uint8_t) line.data[0]));

    // Peel the decimal line number. The ROM would &8D-encode it and have the insert routine decode
    // it straight back into the record header - reading the digits directly lands the same bytes.
    uint32_t pos = 0;
    uint32_t number = 0;
    while (pos < line.len && basic_is_digit((uint8_t) line.data[pos])) {
        if (number <= basic_max_line_number) {              // saturate: only "too big" matters
            number = number * 10 + (uint32_t) (line.data[pos] - '0');
        }
        pos++;
    }
    if (number > basic_max_line_number) {
        return (basic_line_result) {.error = error_type_bad_basic_line_number};
    }

    // Trailing housekeeping the ROM's line-insert does on every entered line: spaces at the end are
    // dropped (a '\r' first, in case the source came with CRLF endings). Spaces after the number
    // stay - stripping those is LISTO's opt-in behaviour and LISTO 0 is the power-on default.
    uint32_t end = line.len;
    if (end > pos && line.data[end - 1] == '\r') {
        end--;
    }
    while (end > pos && line.data[end - 1] == ' ') {
        end--;
    }

    rc_array_bytes out = rc_array_bytes_make(line.len + 16, arena);
    rc_array_bytes_push(&out, 0x0D, arena);
    rc_array_bytes_push(&out, (uint8_t) (number >> 8), arena);
    rc_array_bytes_push(&out, (uint8_t) (number & 0xFF), arena);
    rc_array_bytes_push(&out, 0, arena);                    // the length, filled in once we know it
    basic_tokenise_text(&out, rc_str_substr(line, pos, end - pos), arena);
    if (out.num > basic_max_record) {
        return (basic_line_result) {.error = error_type_basic_line_too_long};
    }
    rc_array_bytes_set(&out, 3, (uint8_t) out.num);
    return (basic_line_result) {.bytes = out.view};
}


#ifdef BARON_TESTS

#include "richc/test.h"
#include "richc/arena.h"
#include "richc/mstr.h"

RC_TEST_GROUP_DATA(basic) {
    rc_arena arena;
};

RC_TEST_GROUP_INIT(basic, fix)
{
    fix->arena = rc_arena_make_default();
}

RC_TEST_GROUP_DEINIT(basic, fix)
{
    rc_arena_deinit(&fix->arena);
}

static bool basic_record_is(rc_str line, const uint8_t *expect, uint32_t num, rc_arena *arena)
{
    basic_line_result r = basic_tokenise_line(line, arena);
    if (r.error != error_type_none || r.bytes.num != num) {
        return false;
    }
    for (uint32_t i = 0; i < num; i++) {
        if (rc_view_bytes_get(r.bytes, i) != expect[i]) {
            return false;
        }
    }
    return true;
}

RC_TEST(basic, table_integrity)
{
    // Tokens have the top bit (that is how the ROM finds the end of each keyword's text), flags do
    // not (that is how it finds the token), first letters never decrease (the early-out), and WIDTH's
    // &FE sits last (the sentinel).
    uint32_t count = sizeof(basic_keywords) / sizeof(basic_keywords[0]);
    bool ok = true;
    for (uint32_t k = 0; k < count; k++) {
        ok = ok && basic_keywords[k].token >= 0x80 && basic_keywords[k].flags < 0x80;
        ok = ok && (k == 0 || basic_keywords[k].text.data[0] >= basic_keywords[k - 1].text.data[0]);
        ok = ok && (basic_keywords[k].token != basic_end_of_table || k == count - 1);
    }
    RC_CHECK_TRUE(ok);
    RC_CHECK(basic_keywords[count - 1].text, ==, RC_STR("WIDTH"));
}

RC_TEST_STEP(basic, records, fix)
{
    // The record is 0D, big-endian line number, whole-record length, text.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10PRINT"),  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x05, 0xF1}, 5, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10 PRINT"), (uint8_t[]) {0x0D, 0x00, 0x0A, 0x06, 0x20, 0xF1}, 6, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("300CLS"),   (uint8_t[]) {0x0D, 0x01, 0x2C, 0x05, 0xDB}, 5, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10"),       (uint8_t[]) {0x0D, 0x00, 0x0A, 0x04}, 4, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10CLS  "),  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x05, 0xDB}, 5, &fix->arena));   // trailing spaces go
    RC_CHECK_TRUE(basic_record_is(RC_STR("10CLS\r"),  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x05, 0xDB}, 5, &fix->arena));   // CRLF leftovers too
}

RC_TEST_STEP(basic, abbreviations, fix)
{
    // Table order decides what a '.' expands to: PRINT is the first P, AND the first A, INPUT the
    // first I. A '.' after a full match is just a full stop.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10P."),     (uint8_t[]) {0x0D, 0x00, 0x0A, 0x05, 0xF1}, 5, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10PRIN."),  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x05, 0xF1}, 5, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10X=1A.2"), (uint8_t[]) {0x0D, 0x00, 0x0A, 0x09, 0x58, 0x3D, 0x31, 0x80, 0x32}, 9, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10I.X"),    (uint8_t[]) {0x0D, 0x00, 0x0A, 0x06, 0xE8, 0x58}, 6, &fix->arena));
}

RC_TEST_STEP(basic, pseudo_variables, fix)
{
    // The +&40 rule: assigning to PAGE is a statement (token &D0), reading it is a function (&90).
    // TIME$ works because '$' is not a variable character, so conditional TIME still matches.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10PAGE=8"), (uint8_t[]) {0x0D, 0x00, 0x0A, 0x07, 0xD0, 0x3D, 0x38}, 7, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10X=PAGE"), (uint8_t[]) {0x0D, 0x00, 0x0A, 0x07, 0x58, 0x3D, 0x90}, 7, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10TIME$=A$"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x09, 0xD1, 0x24, 0x3D, 0x41, 0x24}, 9, &fix->arena));
}

RC_TEST_STEP(basic, conditional_keywords, fix)
{
    // A conditional keyword followed by a variable character is no keyword at all; an unconditional
    // one tokenises even inside a longer name (the classic TOTAL trap).
    RC_CHECK_TRUE(basic_record_is(RC_STR("10COUNTER=1"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0D, 0x43, 0x4F, 0x55, 0x4E, 0x54, 0x45, 0x52, 0x3D, 0x31}, 13, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10TOTAL=1"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0A, 0xB8, 0x54, 0x41, 0x4C, 0x3D, 0x31}, 10, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10TOP=1"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x08, 0xB8, 0x50, 0x3D, 0x31}, 8, &fix->arena));
}

RC_TEST_STEP(basic, line_numbers_embed, fix)
{
    // After GOTO (and at the very start of the text, where the ROM leaves lnexp set) numbers become
    // the &8D three-byte form. The worked examples are straight from the disassembly.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10GOTO 10"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0A, 0xE5, 0x20, 0x8D, 0x54, 0x4A, 0x40}, 10, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10GOTO333"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x09, 0xE5, 0x8D, 0x44, 0x4D, 0x41}, 9, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10GOTO12345"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x09, 0xE5, 0x8D, 0x54, 0x79, 0x70}, 9, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("5GOSUB139"),
                                  (uint8_t[]) {0x0D, 0x00, 0x05, 0x09, 0xE4, 0x8D, 0x74, 0x4B, 0x40}, 9, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10 20"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x09, 0x20, 0x8D, 0x54, 0x54, 0x40}, 9, &fix->arena));
    // An embedded number too big for the form stays as digits (the ROM's fallback), and an ordinary
    // expression number never embeds because something always cleared lnexp first.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10GOTO70000"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0A, 0xE5, 0x37, 0x30, 0x30, 0x30, 0x30}, 10, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10X=10"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x08, 0x58, 0x3D, 0x31, 0x30}, 8, &fix->arena));
}

RC_TEST_STEP(basic, raw_regions, fix)
{
    // REM/DATA keep the rest of the line, '*' at a statement start keeps the whole command, strings
    // are never looked inside, and an unterminated string swallows the remainder.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10REM lda #1"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0C, 0xF4, 0x20, 0x6C, 0x64, 0x61, 0x20, 0x23, 0x31}, 12, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10*FX210,1"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0C, 0x2A, 0x46, 0x58, 0x32, 0x31, 0x30, 0x2C, 0x31}, 12, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10X=1*2"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x09, 0x58, 0x3D, 0x31, 0x2A, 0x32}, 9, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10PRINT\"IF\""),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x09, 0xF1, 0x22, 0x49, 0x46, 0x22}, 9, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10PRINT\"AB"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x08, 0xF1, 0x22, 0x41, 0x42}, 8, &fix->arena));
}

RC_TEST_STEP(basic, statement_state, fix)
{
    // ':' returns to statement start (so a '*' command can follow mid-line, and pseudo-variables go
    // statement-form again); THEN both returns to start AND expects a line number.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10CLS:*CAT"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0A, 0xDB, 0x3A, 0x2A, 0x43, 0x41, 0x54}, 10, &fix->arena));
    RC_CHECK_TRUE(basic_record_is(RC_STR("10IF X THEN 20"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0E, 0xE7, 0x20, 0x58, 0x20, 0x8C, 0x20,
                                               0x8D, 0x54, 0x54, 0x40}, 14, &fix->arena));
    // FN/PROC names are the user's own - never tokenised, even when they contain a keyword.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10PROCtotal"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0A, 0xF2, 0x74, 0x6F, 0x74, 0x61, 0x6C}, 10, &fix->arena));
    // Lowercase keywords are just variables.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10print=1"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0B, 0x70, 0x72, 0x69, 0x6E, 0x74, 0x3D, 0x31}, 11, &fix->arena));
    // '&' literals take uppercase hex only and leave all state alone.
    RC_CHECK_TRUE(basic_record_is(RC_STR("10CALL&FFEE"),
                                  (uint8_t[]) {0x0D, 0x00, 0x0A, 0x0A, 0xD6, 0x26, 0x46, 0x46, 0x45, 0x45}, 10, &fix->arena));
}

RC_TEST_STEP(basic, errors, fix)
{
    basic_line_result r = basic_tokenise_line(RC_STR("70000REM"), &fix->arena);
    RC_CHECK_TRUE(r.error == error_type_bad_basic_line_number);
    RC_CHECK(r.bytes.num, ==, 0u);

    // 4 header bytes + REM + 252 raw characters = 257: one byte too many twice over.
    rc_mstr long_line = rc_mstr_from_cstr("10REM", 300, &fix->arena);
    rc_mstr_append_n(&long_line, 'A', 252, &fix->arena);
    r = basic_tokenise_line(long_line.view, &fix->arena);
    RC_CHECK_TRUE(r.error == error_type_basic_line_too_long);
    RC_CHECK(r.bytes.num, ==, 0u);

    // 251 bytes of text is the largest record that fits.
    rc_mstr max_line = rc_mstr_from_cstr("10REM", 300, &fix->arena);
    rc_mstr_append_n(&max_line, 'A', 250, &fix->arena);
    r = basic_tokenise_line(max_line.view, &fix->arena);
    RC_CHECK_TRUE(r.error == error_type_none);
    RC_CHECK(r.bytes.num, ==, 255u);
    RC_CHECK((uint32_t) rc_view_bytes_get(r.bytes, 3), ==, 255u);
}

#endif // BARON_TESTS
