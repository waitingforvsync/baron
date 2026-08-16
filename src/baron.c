#include "baron.h"

#include "richc/macros.h"


baron baron_make(baron_desc *a)
{
    RC_ASSERT(a != NULL);
    baron b;
    b.permanent = &a->permanent;   // borrowed: the arenas stay the caller's to free
    b.per_pass  = &a->per_pass;

    scopes_init(&b.scopes, &a->permanent);
    scopes_make_root(&b.scopes);              // the root is scope index 0
    sections_init(&b.sections, &a->per_pass, &a->permanent);   // default section (re)made per pass; permanent backs the cross-pass splice sizes
    zeropage_init(&b.zeropage, &a->permanent);   // empty + dormant; run_pass clears it, ZPRESERVE fills it
    source_files_init(&b.source_files, &a->permanent);
    macros_init(&b.macros, &a->per_pass);         // run_pass reseeds its store + token table each pass
    functions_init(&b.functions, &a->per_pass);   // ditto for the operand table

    b.include_depth  = 0;
    b.macro_depth    = 0;
    b.function_depth = 0;
    b.diagnostics    = rc_array_diagnostic_make(256, &a->permanent);
    for (uint32_t i = 0; i < baron_num_channels; i++) {
        b.channels[i] = (rc_mstr) {0};   // empty handles; run_pass re-zeroes them, appends allocate lazily
    }
    b.want_verbose   = a->verbose;      // whether to run the listing pass at all
    return b;
}

// The payload is COPIED into the permanent arena (where the diagnostics themselves live), so a caller may
// hand in a view into scratch, per_pass or source text without a thought for lifetime - diagnostics are
// rare, and the copy buys away a whole class of dangling views.
static rc_str payload_copy(baron *b, rc_str payload)
{
    return payload.len == 0 ? (rc_str) {0} : rc_mstr_from_str(payload, 0, b->permanent).view;
}

void baron_error_payload(baron *b, error_type code, cursor at, rc_str payload)
{
    RC_ASSERT(b != NULL);
    rc_array_diagnostic_push(&b->diagnostics,
        (diagnostic) {.code = code, .at = at, .severity = severity_error, .payload = payload_copy(b, payload)},
        b->permanent);
}

void baron_warning_payload(baron *b, error_type code, cursor at, uint8_t severity, rc_str payload)
{
    RC_ASSERT(b != NULL && severity != severity_error);   // a warning is a positive level; 0 would fail the assemble
    rc_array_diagnostic_push(&b->diagnostics,
        (diagnostic) {.code = code, .at = at, .severity = severity, .payload = payload_copy(b, payload)},
        b->permanent);
}

void baron_error(baron *b, error_type code, cursor at)
{
    baron_error_payload(b, code, at, (rc_str) {0});
}

void baron_warning(baron *b, error_type code, cursor at, uint8_t severity)
{
    baron_warning_payload(b, code, at, severity, (rc_str) {0});
}

uint32_t baron_error_count(const baron *b)
{
    RC_ASSERT(b != NULL);
    uint32_t count = 0;
    for (uint32_t i = 0; i < b->diagnostics.num; i++) {
        if (rc_view_diagnostic_get(b->diagnostics.view, i).severity == severity_error) {
            count++;
        }
    }
    return count;
}

bool baron_has_errors(const baron *b)
{
    return baron_error_count(b) > 0;
}


#ifdef BARON_TESTS

#include "richc/test.h"

RC_TEST(baron, init_set_get)
{
    baron_desc desc = (baron_desc) {
        .permanent = rc_arena_make_default(),
        .per_pass  = rc_arena_make_default(),
        .scratch   = rc_arena_make_default(),
    };
    baron b = baron_make(&desc);

    // baron_make already made the root at scope index 0.
    RC_CHECK_TRUE(scopes_set_symbol(&b.scopes, 0, RC_STR("answer"), value_make_numeric(42.0), (cursor){0, 0}) == symbol_status_unchanged);
    RC_CHECK_TRUE(value_is_equal(scopes_get_symbol(&b.scopes, 0, RC_STR("answer")), value_make_numeric(42.0)));

    rc_arena_deinit(&desc.permanent);
    rc_arena_deinit(&desc.per_pass);
    rc_arena_deinit(&desc.scratch);
}

#endif // BARON_TESTS
