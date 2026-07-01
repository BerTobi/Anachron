ANACHRON - Windows XP quickstart
================================

ANACHRON is a from-scratch, local AI coding agent. It runs entirely on this
machine (no network), driving a small GGUF language model that you provide.


THREE STEPS
  1. Get a model (see below) and drop the .gguf file into the  models\  folder.
  2. Double-click  anachron.exe
  3. Pick your model from the list, answer two quick questions, and you're in.

That's it - no command line needed. On first run ANACHRON asks for the model,
a working folder, and whether to use the faster "lean" prompt, then offers to
save your answers to agent.json so the next launch skips straight to the prompt.


GET A MODEL (required - not included)
  ANACHRON does not ship a model (they are large and separately licensed).
  Download one small GGUF and put it in the  models\  folder:

      qwen2.5-coder-0.5b-instruct-Q8_0.gguf     (about 640 MB)

  Get it from Hugging Face (search: "qwen2.5-coder-0.5b-instruct GGUF"). If this
  PC has no browser, download on another machine and copy the file over.

  IMPORTANT: use a 0.5B model. A 1.5B will NOT fit a 32-bit process, and a model
  quantized below Q8 tends to produce nothing - stick with the 0.5B Q8.


WHAT'S IN THIS FOLDER
  anachron.exe         the program (static PE32; no DLLs, no installer)
  models\              put your .gguf model file(s) here
  grammars\            files the model needs - keep them beside the exe
  work\                the folder the agent reads/writes (its sandbox)
  agent.json.example   a sample settings file (setup writes a real agent.json)


IN A SESSION
  - Type a task and press Enter. /help lists commands, /quit exits.
  - Before writing a file or running a command it asks:  [y/N]   (Enter = No.)
  - /model         switch models - lists what's in models\ and lets you pick.
  - @path\to\file  attach a file to your message.
  - The FIRST turn is slow (reads the whole prompt). After that a cache file is
    written next to the model and reused, so later runs start in seconds.


PREFER THE COMMAND LINE?  (cmd.exe, in this folder)
  anachron.exe --model models\qwen2.5-coder-0.5b-instruct-Q8_0.gguf --sandbox work
  Flags:  --lean (faster first turn)   --yolo (skip the [y/N] gate)   --no-color
  Env:    ANACHRON_THREADS=N  (default 4; a single-core machine -> 1)


Full docs, source, and updates:  https://github.com/BerTobi/Anachron
