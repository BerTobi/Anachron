# strutil — ANACHRON demo workspace

A small C99 string library, here so you can try the agent's discovery, edit, and
session features against a real (if tiny) codebase. Conventions are in AGENTS.md,
which the agent loads into its system prompt automatically.

## Things to try in the REPL (`./run.sh --hammer` from the repo root)

- `search str_`            — find every use of the API (the *.log and build/ files are
                             skipped via .gitignore)
- `glob *.c`               — list the source files
- `@strutil.c explain str_trim`   — attach a file inline with @
- ask it to make `str_upper` return NULL for a NULL input — watch the coloured diff
- `/save demo` · `/sessions` · `/new` · `/resume demo`   — session persistence
- `/undo`                  — revert the last edit from its .anbak snapshot
- `/model <path-to.gguf>`  — hot-swap the model mid-conversation

Build/run the demo yourself with:  `cc -std=c99 -Wall *.c -o demo && ./demo`
