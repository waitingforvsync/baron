/**
 *  @file   main.c
 * 
 *  Implementation of the command line tool wrapping baronlib
 */

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "baron/baron.h"


#define BARON_VERSION "0.1"


void display_version(void) {
    puts("baron " BARON_VERSION);
    puts("Developed and maintained by Rich Talbot-Watkins <richtw1@gmail.com>");
}


void display_help(void) {
    puts("Usage: baron [OPTION]... [FILE]...");
    puts("A 6502 assembler targetting the BBC Micro.");
    puts("");
    puts("  -C, --cmos       Enable CMOS instructions");
    puts("  -D <defines>     Define the variables in the comma-separated list which follows");
    puts("                   Example: -D SecondProc=TRUE,thickness=2,version=\"1.0\"");
    puts("  -log<N> <file>   Output messages to stream N (1-7) to the given file");
    puts("  -O <path>        Specify path for outputting object files");
    puts("  -opt <val>       When generating a disk image, set this boot option");
    puts("  -ssd <file>      Generate a disk image with the given filename");
    puts("  -t <title>       When generating a disk image, set this disk title");
    puts("  -v, --verbose    Output listing for assembled source code");
    puts("");
    puts("  --version to display version and author information");
    puts("  --help to display this help again");
}


int main(int argc, char *argv[]) {

    const char *input_filename = 0;
    const char *log_filenames[8] = {0};
    const char *output_path = 0;
    const char *output_ssd = 0;
    const char *defines = 0;
    bool cmos = false;
    bool verbose = false;
    int opt = 0;
    const char *title = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            display_help();
        }
        else if (strcmp(argv[i], "--version") == 0) {
            display_version();
        }
        else if (strcmp(argv[i], "-C") == 0 || strcmp(argv[i], "--cmos") == 0) {
            cmos = true;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        }
        else if (strcmp(argv[i], "-D") == 0) {
            if (++i < argc) {
                defines = argv[i];
            }
            else {
                fprintf(stderr, "Missing defines list (-D <comma-separated list>)\n");
                return EXIT_FAILURE;
            }
        }
        else if (strcmp(argv[i], "-O") == 0) {
            if (++i < argc) {
                output_path = argv[i];
            }
            else {
                fprintf(stderr, "Missing output path filename (-O <path>)\n");
                return EXIT_FAILURE;
            }
        }
        else if (strcmp(argv[i], "-ssd") == 0) {
            if (++i < argc) {
                output_ssd = argv[i];
            }
            else {
                fprintf(stderr, "Missing ssd filename (-ssd <filename>)\n");
                return EXIT_FAILURE;
            }
        }
        else if (strcmp(argv[i], "-opt") == 0) {
            if (++i < argc) {
                if (argv[i][0] >= '0' && argv[i][0] <= '3' && argv[i][1] == 0) {
                    opt = argv[i][0] - '0';
                }
                else {
                    fprintf(stderr, "Invalid disk option (-opt <0-3>)\n");
                    return EXIT_FAILURE;
                }
            }
            else {
                fprintf(stderr, "Missing disk option (-opt <0-3>)\n");
                return EXIT_FAILURE;
            }
        }
        else if (strcmp(argv[i], "-t") == 0) {
            if (++i < argc) {
                title = argv[i];
            }
            else {
                fprintf(stderr, "Missing disk title (-t <title>)\n");
                return EXIT_FAILURE;
            }
        }
        else if (strncmp(argv[i], "-log", 4) == 0) {
            if (argv[i][4] >= '1' && argv[i][4] < '8' && argv[i][5] == 0) {
                if (++i < argc) {
                    log_filenames[argv[i - 1][4] - '0'] = argv[i];
                }
                else {
                    fprintf(stderr, "Missing log filename (-log%c <filename>)\n", argv[i - 1][4]);
                    return EXIT_FAILURE;
                }
            }
            else {
                fprintf(stderr, "Invalid log number: %s\n", argv[i]);
                return EXIT_FAILURE;
            }

        }
    }

    baron_desc_t desc = {
        .allocator = 0
    };

    (void)(input_filename);
    (void)(desc);


    return EXIT_SUCCESS;
}
