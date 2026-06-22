# Project: strutil

A tiny C99 string-utility library — the demo workspace for trying out ANACHRON.

## Conventions
- C99, no external dependencies. Public functions are prefixed `str_` and return
  newly malloc'd strings the caller is responsible for freeing.
- Every public function gets a one-line doc comment in `strutil.h`.
- Keep implementations in `strutil.c`; the public API lives in `strutil.h`.
- Build and run with:  `cc -std=c99 -Wall *.c -o demo && ./demo`

## Layout
- `strutil.h`  — public API
- `strutil.c`  — implementations
- `main.c`     — a small program that exercises the library
