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


BIGGER BRAINS: REMOTE INFERENCE AND HOSTED APIS
  The local 0.5B model is the floor, not the ceiling. The --model setting (or the
  setup prompt, or /model in a session) also accepts:

  http://gpu-box:8080
      A llama.cpp "llama-server" running on another machine on your LAN - run a
      7B/14B/70B there and this PC becomes a thin client. Plain HTTP, so it works
      on real XP with no TLS trouble. On the big machine:
          llama-server -m big-model.gguf --host 0.0.0.0 --port 8080
      If the server uses --api-key, set ANACHRON_REMOTE_KEY (or "remote_key" in
      agent.json) here.

  anthropic:claude-opus-4-8
      The Anthropic API. Needs ANACHRON_API_KEY (or "api_key" in agent.json).
      NOTE: api.anthropic.com needs TLS 1.2 - stock XP SP3 cannot reach it
      (POSReady-patched systems may). The LAN option above always works.

  openai:MODEL_NAME
      Any OpenAI-compatible /v1/chat/completions endpoint. Point ANACHRON_API_URL
      (or "api_url" in agent.json) at a LAN server (llama-server, LM Studio,
      Ollama) - then no key is needed - or leave it default for api.openai.com.

  Everything else stays the same: the [y/N] gate, /files, the transcript, the
  sandbox. Only the brain moves.


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
  - /files         what changed this session (+added -removed per file).
  - /update        update ANACHRON itself (see UPDATING below).
  - @path\to\file  attach a file to your message.
  - !dir           a line starting with ! runs as a shell command (no model).
  - A line ending in \  continues on the next line (multiline input).
  - While the model computes you'll see  [####......] 47s 0.2t/s  tick after the
    text: the current token's progress, elapsed time, and speed. Not frozen!
  - The FIRST turn is slow (reads the whole prompt). After that a cache file is
    written next to the model and reused, so later runs start in seconds.


UPDATING
  Type  /update  in a session. It tries two things, newest wins:
  1. github.com - works if this machine's Windows can speak modern TLS
     (stock XP SP3 cannot; POSReady-patched systems often can).
  2. An  updates\  folder next to anachron.exe: on any machine, download the
     newest  anachron-<version>-winxp.exe  from
     github.com/BerTobi/Anachron/releases, drop it in updates\, run /update.
  Either way it checks the new exe actually runs, swaps it in, and tells you to
  restart. Your model, settings and work folder are untouched.


PREFER THE COMMAND LINE?  (cmd.exe, in this folder)
  anachron.exe --model models\qwen2.5-coder-0.5b-instruct-Q8_0.gguf --sandbox work
  Flags:  --lean (faster first turn)   --yolo (skip the [y/N] gate)   --no-color
  Env:    ANACHRON_THREADS=N  (default 4; a single-core machine -> 1)


Full docs, source, and updates:  https://github.com/BerTobi/Anachron
