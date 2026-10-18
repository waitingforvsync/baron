# Specifying section transforms / compression

## The pitch

Now that sections own their own bytes, I would love to be able to transform that data in a user-defined way, without having to lean on an external tool to do it.

Right now, if you wanted to do that, you would call Baron twice:
- Assemble the sections you want to transform in some way, invoking Baron with `-p` to save the sections to the host path.
- Transform them with your tool (compression, encryption, whatever)
- Make a disk image with Baron (`-o`), using `INCBIN` to include the transformed binaries as payloads to your BBC Micro executable which performs the decompression or decryption.

The problem with this is that it's a clunky workflow, and also, from one Baron invocation to the next, you lose the symbols. So you would then have to "export" them in some way, e.g. PRINT #n the addresses you care about in the first assemble, redirect that to a source file, and include that in the second assemble.

## There must be a better way?

At first I wondered about the possibility of having a section attribute which will just spin off a named tool with a file as input and expecting a file as output, which can then be consumed further down the Baron pipeline.

But I would love to be able to do this natively in Baron.

The problem is, how can we provide the flexibility to be able to do fairly arbitrary transforms?

## Section transform spec

My idea is to provide a new block type in Baron (maybe `SPEC <name>`...`ENDSPEC`) which defines how a section should be transformed.

This is what I would like us to design together. It might not be possible in exactly the form I'm envisaging, so let's discuss it.

### Motivating example ###

Suppose we have the following:
```
SPEC MyCompress
   ;... syntax to be decided
ENDSPEC

    SECTION Loader, org=&3000, filename="CODE", exec=entry

    .compressed
    SECTION Code, org=&400, transform="MyCompress"

    .start
        ; the real code

    ENDSECTION

.entry
    LDX #LO(compressed)
    LDY #HI(compressed)
    JSR my_decompress
    JMP start

.my_decompress
    ; whatever

ENDSECTION
```

The idea is that the Code section will be compressed according to the spec of MyCompress and will hold those transformed bytes (which are then of course copied to the parent Loader section).

What I'd love to be able to do is describe the transform in terms of how the decompressor handles it, and have Baron work out the best way to compress the data to that spec. It would be natively aware of certain concepts like "best run of N bytes at offset O".

So, e.g. given the following spec for decompressing the data (described for now in natural language), could this be described with a more rigid syntax, and could Baron compress the data to that spec?

- read 16 bits (call it LEN)
- read 3 bits (call it OFFSET_BITS)
- LET literal = TRUE
- WHILE output length < LEN
    - IF literal
        - read elias gamma value (call it NUM)
        - FOR n=0..<NUM
            - read 8 bits (call it D)
            - output D
        - NEXT
    - ELSE
        - read elias gamma value (call it NUM)
        - FOR n=0..<NUM
            - read elias gamma value (call it REP)
            - read elias gamma value (call it HI)
            - read OFFSET_BITS bits (call it LO)
            - LET o = LO + ((HI-1) << OFFSET_BITS) + 1
            - FOR m=0..REP
                - output byte from output buf[current - o]
            - NEXT
        - NEXT
    - ENDIF
    - literal = NOT literal
- ENDWHILE

While this is probably unattainable, and would probably need to be specified from the perspective of the compressor, with higher-level constructs, I think it's a good starting point for discussion. Do you understand the compression spec I want to describe with this? How might we rephrase this to describe the compression algorithm? Do you think a feature like this can work?
