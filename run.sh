#!/bin/sh
# Launch the ANACHRON agent with sensible defaults.
# Usage:  ./run.sh               interactive conversation (0.5B Qwen-Coder, fast)
#         ./run.sh --big         1.5B Qwen-Coder (better code, ~5x slower)
#         ./run.sh --hammer      Hammer-0.5B function-calling fine-tune (best tool use)
#         ./run.sh "task..."     run one task and exit
#         ./run.sh --hammer "…"  combine
# Override with env: ANACHRON_MODEL=... ANACHRON_SANDBOX=... ANACHRON_THREADS=...
set -e
cd "$(dirname "$0")"

MODEL_SMALL=spike-phase0/models/qwen2.5-coder-0.5b-instruct-q8_0.gguf
MODEL_BIG=spike-phase0/models/qwen2.5-coder-1.5b-instruct-q8_0.gguf
MODEL_HAMMER=spike-phase0/models/hammer2.0-0.5b-q8_0.gguf

# Model selection (unless ANACHRON_MODEL is set explicitly):
#   --big/-b    -> 1.5B Qwen-Coder (better code, slower)
#   --hammer/-H -> Hammer-0.5B, a function-calling fine-tune: far more reliable at
#                  actually USING tools (write/run), at 0.5B speed. Best for agentic
#                  tasks; base Qwen-Coder is a touch better at pure code-writing.
MODEL=${ANACHRON_MODEL:-$MODEL_SMALL}
case "$1" in
    --big|-b)    MODEL=${ANACHRON_MODEL:-$MODEL_BIG};    shift ;;
    --hammer|-H) MODEL=${ANACHRON_MODEL:-$MODEL_HAMMER}; shift ;;
esac

SANDBOX=${ANACHRON_SANDBOX:-./workspace}

# Build the binary the first time (or after edits if it's missing).
[ -x ./anachron-llama ] || make llama

mkdir -p "$SANDBOX"
echo "model:   $MODEL"
echo "sandbox: $SANDBOX  (the agent can only touch files here)"
exec ./anachron-llama --model "$MODEL" --sandbox "$SANDBOX" "$@"
