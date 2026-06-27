# Baron #
A portable 6502 assembler designed for BBC Micro development, the spiritual successor to BeebAsm.

## About ##
Baron is a cross-platform 6502 assembler, designed to make developing for the BBC Micro easy, with a familiar syntax inspired by BBC BASIC's built-in assembler. It can output conventional object code files, or emulator-ready DFS disk images.

BeebAsm 1.x was hastily written many years ago with one goal: to allow BBC BASIC assembler programs to be trivially ported to a modern PC environment, requiring as few changes as possible. This required it to support as many of BBC BASIC's features and conventions as possible, some of which feel increasingly anachronistic today.

Baron is a ground-up redesign and rewrite of BeebAsm, in response to some of the limitations of the original BeebAsm. While much of the original syntax is retained, breaking free of the constraints of BBC BASIC means that a number of new features can be accommodated in a modern style, including strings, lists, named scopes, assisted memory allocation, profiling, user-defined functions, and much more.

## Building ##
Baron is written in C17 and builds with [richc](https://github.com/waitingforvsync/richc) as its foundation library, included as a git submodule. It's tested with Clang, CMake and Ninja but will hopefully build with any major compiler.

Clone with submodules, or initialise them in an existing clone:

```
git clone --recurse-submodules https://github.com/waitingforvsync/baron.git
# or, after a plain clone:
git submodule update --init
```

Configure and build:

```
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang
cmake --build build
```

The unit tests are compiled into the `baron` executable and run automatically as a post-build step, so a failing test fails the build. To run them by hand, optionally filtered to a single group:

```
./build/src/baron --test          # all tests
./build/src/baron --test value    # just the "value" group
```

## Why "Baron"? ##
Since there are breaking changes to syntax and usage compared to current versions of BeebAsm, this no longer feels like a BeebAsm 2.0. So instead, with allusions to that Acorn folklore staple, the Baron of Beef pub in Cambridge, it has been retitled Baron.

It's an acronym for Beeb Asm, Ready or Not.

Honest.
