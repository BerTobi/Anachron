ANACHRON - Windows XP quickstart
================================

ANACHRON is a from-scratch, local AI coding agent. It runs entirely on this
machine (no network), driving a small GGUF language model that you provide.


WHAT'S IN THIS FOLDER
  anachron.exe         the program (static PE32; no DLLs, no installer)
  grammars\            tool-call grammars the model needs - keep beside the exe
  agent.json.example   optional settings; copy to "agent.json" to use them
  README.txt           this file


YOU ALSO NEED A MODEL FILE
  ANACHRON does not ship a model (they are large and separately licensed).
  Download one small GGUF and drop it in this folder. Recommended for XP:

      qwen2.5-coder-0.5b-instruct-Q8_0.gguf     (about 640 MB)

  Get it from Hugging Face (search: "qwen2.5-coder-0.5b-instruct GGUF"). If the
  XP box has no browser, download on another PC and copy the file over.

  IMPORTANT: use a 0.5B model. A 1.5B model will NOT fit a 32-bit process, and a
  model quantized below Q8 tends to produce nothing - stick with the 0.5B Q8.


RUN IT  (open cmd.exe, cd into this folder)

      anachron.exe --model qwen2.5-coder-0.5b-instruct-Q8_0.gguf --sandbox work

  "work" is a folder the agent may read and write; it cannot escape it. Then
  type a task and press Enter. Type /help for commands, /quit to leave.


WHAT TO EXPECT
  - Before writing a file or running a command it asks:  [y/N]   (Enter = No.)
        Skip the prompt entirely with  --yolo   (or set ANACHRON_YOLO=1)
  - The FIRST turn is slow (it reads the whole prompt - minutes on a Pentium-M).
    After that a cache file is written next to the model and reused, so later
    sessions start in seconds. For a faster first turn:  set ANACHRON_LEAN=1
  - Colour is on automatically; turn it off with  --no-color
  - Attach a file to your message with  @path\to\file


USEFUL ENVIRONMENT VARIABLES  (set NAME=value  before running)
  ANACHRON_LEAN=1      shorter prompt -> faster first turn (slightly terser)
  ANACHRON_THREADS=N   CPU threads (default 4; a single-core machine -> 1)
  ANACHRON_YOLO=1      skip the [y/N] permission gate
  ANACHRON_MMAP=1      alternate model-loading path (try if a model won't load)


Full docs, source, and updates:  https://github.com/BerTobi/Anachron
