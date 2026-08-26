# Baron #
A portable 6502 assembler designed for BBC Micro development, the spiritual successor to [BeebAsm](https://github.com/stardot/beebasm).

## About ##
Baron is a cross-platform 6502 assembler, designed to make developing for the BBC Micro easy, with a familiar syntax inspired by BBC BASIC's built-in assembler. It can output conventional object code files, or emulator-ready DFS disk images.

I hastily wrote BeebAsm 1.x many years ago with one goal: to allow BBC BASIC assembler programs to be trivially ported to a modern PC environment, requiring as few changes as possible. This required it to support as many of BBC BASIC's features and conventions as possible, some of which feel really anachronistic today.

Baron is a ground-up redesign and rewrite of BeebAsm, in response to some of its problems which annoy me the most. Much of the original familiar syntax is retained, but looking to Python and elsewhere for inspiration, there are now a number of new features in a modern style, such as strings, lists, named scopes, assisted memory allocation, user-defined functions, and more.

## Building ##
Baron is written in C17 and builds with [richc](https://github.com/waitingforvsync/richc) as its foundation library, included as a git submodule. It's tested with clang/gcc on Linux, and Visual Studio on Windows, with CMake and Ninja, but should build with any major compiler.

Clone with submodules, or initialise them in an existing clone:

```
git clone --recurse-submodules https://github.com/waitingforvsync/baron.git
# or, after a plain clone:
git submodule update --init
```

Configure and build an optimised binary, e.g.:

```
cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Visual Studio for Windows has built-in CMake support: just open the folder containing the root CMakeLists.txt.

Unit tests are off by default. Turn them on with the CMake variable `BARON_TESTS`: they are compiled into the `baron` executable and run automatically as a post-build step.

## Getting started ##
Go and check out The Baron Guide, [here](docs/guide.md). A more po-faced reference can be found [here](docs/reference.md).

There is a [Baron BBC Micro Assembler](https://marketplace.visualstudio.com/items?itemName=RichTalbot-Watkins.baron-vsc) extension in Visual Studio Code Marketplace, which is [developed](https://github.com/waitingforvsync/baron-vsc) in parallel with Baron.

## Why "Baron"? ##
Since there are breaking changes to syntax and usage compared to current versions of BeebAsm, this no longer feels like a BeebAsm 2.0. So instead, with allusions to that Acorn folklore staple, the Baron of Beef pub in Cambridge, it has been retitled Baron.

It's an acronym for Beeb Asm, Ready or Not.

Honest.

## Licence ##
Baron is released under the [MIT licence](LICENSE), as is [richc](https://github.com/waitingforvsync/richc), the library it is built on.

## Authorship / AI disclosure ##
Baron was started back in 2020 by me, Rich Talbot-Watkins, and lay abandoned for a while until AI assistance helped get it over the line. I designed and implemented most of it, with Claude Code doing the magical zp allocator and other bits, writing full test coverage and the documentation.

Thanks are due to the BBC Micro community at [stardot](https://stardot.org.uk), who kept these machines - and the tools around them - very much alive.
