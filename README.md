# Baron #
A portable 6502 assembler designed for BBC Micro development, the spiritual successor to [BeebAsm](https://github.com/stardot/beebasm).

## About ##
Baron is a cross-platform 6502 assembler, using syntax reminiscent of BBC BASIC's built-in assembler. It can output conventional object code files, or emulator-ready DFS disk images.

Baron is a ground-up rewrite of BeebAsm, incorporating many of its features and syntax, while adding new functionality and fixing some of its perennial irrations and limitations.

#### Why Baron? ####
Since there are breaking changes to syntax compared to BeebAsm, I can't really release this as BeebAsm 2.0. So instead, with allusions to that Acorn folklore staple, the [Baron of Beef](https://www.upi.com/Archives/1984/12/24/Their-bits-are-worse-than-their-bytes/9853472712400/) pub in Cambridge, it has been retitled Baron.

It's an acronym for Beeb Asm, Ready or Not.

Honest.

## Building Baron ##
The hope is that Baron should be easily buildable on any platform with a compliant C11 compiler. That's 13 years old, we're not talking bleeding edge features here. It has been tested on Visual Studio 2022, and recent releases of clang and GCC under Windows. Just let [CMake](https://cmake.org/) do the work.

## Differences between Baron and BeebAsm ##
Assuming that most people trying out Baron will have come from BeebAsm, it's probably useful to understand how they differ, as well as what hasn't changed. Here's a brief overview; we'll dig into the details later.

#### Syntax ####
Syntax-wise, little has changed. Baron still supports multi-line assembly (separating opcodes with colons), conditional assembly (`IF`..`ELIF`..`ELSE`..`ENDIF`), `FOR`..`NEXT` loops, macros, full numerical and string function support, and scoped blocks. In addition, there is also provision for list primitives, automatic zero-page allocation, named scopes with hierarchical labels, macros which can be overloaded with different addressing modes, and overlays.

#### Overlays ####
Overlays are probably the biggest change to the 'philosophy' of Baron, compared to BeebAsm.

BeebAsm treated the 64k address space as a kind of canvas which could be assembled into, saved, cleared and reused. Baron disposes of this idea in favour of overlays, which work rather more similarly to typical modern assemblers (where 'sections' are a similar concept).

Baron assembles code sequentially into whichever overlay you last specified. The instruction pointer (`ORG`) doesn't specify the location in the overlay, just the address that should be used by the assembler. Multiple changes to `ORG` result in contiguous object code (although `SKIP` and `FILLTO` will assemble as many bytes as necessary into the overlay, in order to reach the desired address).

Overlays have names and exist in parallel, and their output may or may not be saved. Any variable defined in the global scope is visible across all overlays. This allows us, for example, to create a non-saved overlay where we define our memory locations using labels, and use this across all code overlays.

Overlays may also be inserted into another overlay. This allows us to assemble code at the address it will be executed at in one overlay, and then inserted into a loader overlay, ready to be relocated down.

If all this sounds complicated, there'll be examples later!
