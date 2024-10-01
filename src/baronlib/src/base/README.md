# base #

Support library providing basic functionality on top of the C standard library.

## Features ##

- Allocation support

  All functions which expect to have to allocate heap memory require an explicit allocation strategy specifying. A simple interface for allocation is defined, and a number of implementations, including dynamic and fixed arenas, pools and fixed buffers.

- String handling

  String handling in C is a pain. With this library, string views (modelled as a {pointer, length}) are value types, while managed strings which maintain their own buffer are separately supported. A large number of string operations are supported, including parsing to/from numeric values, splitting by delimiter strings, string slicing, string matching, and much more.

- Array support

  Create and build arrays and slices with a selection of macros wrapping some generic functions.

- File handling

  Basic wrapper around stdio for handling saving and loading of binary files.
  
- Math

  Support for 2D and 3D math (vectors, matrices, quaternions) as well as other primitive types such as AABBs, rays, line segments and so on.

- Unit testing

  Simple unit testing with optional fixtures for TDD.

## Naming conventions ##

All type and function names are `lower_snake_case`.

### Types and functions ###

Types are written thus: `<type>_t`. The defining struct has a corresponding typedef, so the `struct` qualifier can be omitted.

Functions which operate on a given type have a consistent naming convention:

To make an instance of a type, returning it by value, use `make_<type>(...)`. If there is more than way to construct a particular type, it will be qualified in the name, e.g. `make_<type>_with_name(...)`.

To initialize a type instance, `<type>_init(...)`, passing a pointer to the instance to be initialized. If the type requires that heap allocations be made, they will be done here (using the allocator object supplied).

To deinitialize a type instance, `<type>_deinit(...)`, passing a pointer to the instance to be deinitialized. Any allocations made during initialization will be freed here.

Functions which query a type instance take the form `<type>_is_valid()`, passing a pointer to the instance to be queried.

Functions which operate on a type instance as the primary object take the form `<type>_verb()`, passing a pointer to the instance to be operated on.

### Macros ###

Macros are generally in `ALL_CAPS`.

An exception to this is any macro which is "masquerading" as a function, e.g. some of the array support operations. For visual consistency, they are written like functions. Users shouldn't especially need to care that they are macros (probably they are only implemented as such in order to fake generics support).
