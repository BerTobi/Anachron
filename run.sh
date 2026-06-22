#!/bin/sh
# Launch the ANACHRON agent with sensible defaults.
# Usage:  ./run.sh               interactive conversation (0.5B Qwen-Coder, fast)
#         ./run.sh --hammer      Hammer 2.0 0.5B function-calling fine-tune (best tool use)
#         ./run.sh --hammer-big  Hammer 2.0 1.5B (reliable tool use, better code, slow)
#         ./run.sh --hammer21[-big]  Hammer 2.1 0.5B/1.5B (does NOT fit our tool format)
#         ./run.sh --big         1.5B Qwen-Coder (better code, ~5x slower)
#         ./run.sh "task..."     run one task and exit
#         ./run.sh --hammer "…"  combine
# Override with env: ANACHRON_MODEL=... ANACHRON_SANDBOX=... ANACHRON_THREADS=...
set -e
cd "$(dirname "$0")"

MODEL_SMALL=spike-phase0/models/qwen2.5-coder-0.5b-instruct-q8_0.gguf
MODEL_BIG=spike-phase0/models/qwen2.5-coder-1.5b-instruct-q8_0.gguf
MODEL_HAMMER=spike-phase0/models/hammer2.0-0.5b-q8_0.gguf
MODEL_HAMMER_BIG=spike-phase0/models/hammer2.0-1.5b-q8_0.gguf
MODEL_HAMMER21=spike-phase0/models/hammer2.1-0.5b-q8_0.gguf
MODEL_HAMMER21_BIG=spike-phase0/models/hammer2.1-1.5b-q8_0.gguf

# Model selection (unless ANACHRON_MODEL is set explicitly):
#   --hammer/-H     -> Hammer 2.0 0.5B: the reliable, fast 0.5B tool-caller for ANACHRON's
#                      <tool_call> format. Best default for agentic use.
#   --hammer-big    -> Hammer 2.0 1.5B: same reliable tool-calling, better code; much
#                      slower (a cold turn is minutes on CPU). Still the 2.0 family.
#   --hammer21 / --hammer21-big -> Hammer 2.1 0.5B / 1.5B. NOTE: this newer fine-tune is
#                      specialized to its OWN tool format and does NOT emit ANACHRON's
#                      <tool_call> at either size (it prints code/prose instead). Kept
#                      available for experimentation; not recommended for agentic use here.
#   --big/-b        -> 1.5B Qwen-Coder (better pure code, slower).
MODEL=${ANACHRON_MODEL:-$MODEL_SMALL}
case "$1" in
    --big|-b)         MODEL=${ANACHRON_MODEL:-$MODEL_BIG};            shift ;;
    --hammer|-H)      MODEL=${ANACHRON_MODEL:-$MODEL_HAMMER};         shift ;;
    --hammer-big)     MODEL=${ANACHRON_MODEL:-$MODEL_HAMMER_BIG};     shift ;;
    --hammer21)       MODEL=${ANACHRON_MODEL:-$MODEL_HAMMER21};       shift ;;
    --hammer21-big)   MODEL=${ANACHRON_MODEL:-$MODEL_HAMMER21_BIG};   shift ;;
esac

SANDBOX=${ANACHRON_SANDBOX:-./workspace}

# Build the binary the first time (or after edits if it's missing).
[ -x ./anachron-llama ] || make llama

mkdir -p "$SANDBOX"
echo "model:   $MODEL"
echo "sandbox: $SANDBOX  (the agent can only touch files here)"
exec ./anachron-llama --model "$MODEL" --sandbox "$SANDBOX" "$@"
