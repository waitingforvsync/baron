#ifndef BARON_VERSION_H_
#define BARON_VERSION_H_

// Baron's version, for the CLI and the Windows resource script alike. We can't count on the resource
// compiler to join string literals, so the string is spelt out beside the numbers: bump them together.
#define BARON_VERSION_MAJOR 0
#define BARON_VERSION_MINOR 5
#define BARON_VERSION_PATCH 1
#define BARON_VERSION_BUILD 0
#define BARON_VERSION       "0.5.1.0"


#endif // ifndef BARON_VERSION_H_
