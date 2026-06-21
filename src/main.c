#ifdef BARON_TESTS
#include "richc/test.h"
#include <string.h>
#endif


// No command line yet: in a test build, `baron --test [filter]` runs the unit
// tests (the build's POST_BUILD step does exactly this). Otherwise a no-op.
int main(int argc, char **argv)
{
#ifdef BARON_TESTS
    if (argc > 1 && strcmp(argv[1], "--test") == 0) {
        return rc_test_run(argc > 2 ? argv[2] : "");
    }
#endif
    (void)argc;
    (void)argv;
    return 0;
}
