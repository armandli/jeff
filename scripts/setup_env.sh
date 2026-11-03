#!/usr/bin/env bash
# Set up the jeff Python environment for Qwen3-8B QLoRA training on an NVIDIA GPU.
#
# Usage:
#   bash scripts/setup_env.sh [--flash-attn] [--cuda 12.4]
#
# The script:
#   1. Creates a uv virtual-env at python/.venv
#   2. Detects (or accepts) the CUDA version and installs the matching PyTorch wheel
#   3. Installs all project dependencies via uv
#   4. Optionally installs flash-attn (takes ~20 min if compiled from source)
#
# After this script completes, activate with:
#   source python/.venv/bin/activate

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PYTHON_DIR="${REPO_ROOT}/python"

INSTALL_FLASH_ATTN=false
CUDA_OVERRIDE=""

# ---------------------------------------------------------------------------
# Parse args
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --flash-attn) INSTALL_FLASH_ATTN=true ;;
        --cuda)       CUDA_OVERRIDE="$2"; shift ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Detect CUDA version
# ---------------------------------------------------------------------------
detect_cuda() {
    if [[ -n "${CUDA_OVERRIDE}" ]]; then
        echo "${CUDA_OVERRIDE}"
        return
    fi

    if command -v nvcc &>/dev/null; then
        nvcc --version | grep -oP 'release \K[0-9]+\.[0-9]+'
    elif command -v nvidia-smi &>/dev/null; then
        nvidia-smi | grep -oP 'CUDA Version: \K[0-9]+\.[0-9]+'
    else
        echo ""
    fi
}

CUDA_VERSION="$(detect_cuda)"

if [[ -z "${CUDA_VERSION}" ]]; then
    echo "ERROR: Could not detect CUDA. Is nvidia-smi or nvcc on PATH?"
    echo "       Pass --cuda <version> (e.g. --cuda 12.4) to override."
    exit 1
fi

# Map e.g. "12.4" → "cu124", "11.8" → "cu118"
CUDA_MAJOR="${CUDA_VERSION%%.*}"
CUDA_MINOR="${CUDA_VERSION#*.}"
CUDA_MINOR="${CUDA_MINOR%%.*}"   # strip any trailing .x
CUDA_TAG="cu${CUDA_MAJOR}${CUDA_MINOR}"

echo "Detected CUDA ${CUDA_VERSION} → torch index: ${CUDA_TAG}"

# Supported CUDA tags for PyTorch; warn if unusual.
KNOWN_TAGS=("cu118" "cu121" "cu124" "cu126" "cu128")
if [[ ! " ${KNOWN_TAGS[*]} " =~ " ${CUDA_TAG} " ]]; then
    echo "WARNING: ${CUDA_TAG} is not in the known-good list (${KNOWN_TAGS[*]})."
    echo "         Attempting anyway; if PyTorch install fails, pass --cuda <version>."
fi

TORCH_INDEX="https://download.pytorch.org/whl/${CUDA_TAG}"

# ---------------------------------------------------------------------------
# Check uv is available
# ---------------------------------------------------------------------------
if ! command -v uv &>/dev/null; then
    echo "uv not found. Installing via the official installer..."
    curl -LsSf https://astral.sh/uv/install.sh | sh
    # shellcheck disable=SC1090
    source "${HOME}/.local/bin/env" 2>/dev/null || export PATH="${HOME}/.local/bin:${PATH}"
fi

# ---------------------------------------------------------------------------
# Create virtual environment
# ---------------------------------------------------------------------------
VENV="${PYTHON_DIR}/.venv"
echo "Creating virtual environment at ${VENV}..."
uv venv "${VENV}" --python 3.11

# Activate so subsequent uv pip calls use it
export VIRTUAL_ENV="${VENV}"
export PATH="${VENV}/bin:${PATH}"

# ---------------------------------------------------------------------------
# Install PyTorch with CUDA support
# ---------------------------------------------------------------------------
echo "Installing PyTorch for ${CUDA_TAG} from ${TORCH_INDEX}..."
uv pip install \
    "torch>=2.3" \
    "torchvision" \
    "torchaudio" \
    --index-url "${TORCH_INDEX}"

# ---------------------------------------------------------------------------
# Install project dependencies
# ---------------------------------------------------------------------------
echo "Installing jeff project dependencies..."
cd "${PYTHON_DIR}"
uv pip install -e ".[dev]" --no-build-isolation

# ---------------------------------------------------------------------------
# Verify GPU is visible
# ---------------------------------------------------------------------------
echo "Verifying GPU access..."
python - <<'EOF'
import torch
if not torch.cuda.is_available():
    print("WARNING: torch.cuda.is_available() is False — GPU not visible after install.")
else:
    n = torch.cuda.device_count()
    names = [torch.cuda.get_device_name(i) for i in range(n)]
    print(f"OK: {n} GPU(s) found: {names}")
    mem = torch.cuda.get_device_properties(0).total_memory / 2**30
    print(f"   GPU 0 VRAM: {mem:.1f} GB")
    print(f"   Qwen3-8B QLoRA (4-bit) needs ~8 GB; full bf16 needs ~16 GB.")
EOF

# ---------------------------------------------------------------------------
# Optional: flash-attn
# ---------------------------------------------------------------------------
if [[ "${INSTALL_FLASH_ATTN}" == "true" ]]; then
    echo ""
    echo "Installing flash-attn (may take 15-25 min if compiled from source)..."
    # Prefer pre-built wheels; --no-build-isolation is required
    uv pip install flash-attn --no-build-isolation || {
        echo "Pre-built wheel not found; falling back to source build (slow)..."
        uv pip install flash-attn --no-build-isolation --no-binary flash-attn
    }
    python -c "import flash_attn; print(f'flash-attn {flash_attn.__version__} OK')"
else
    echo ""
    echo "Skipping flash-attn (add --flash-attn to enable; recommended for Qwen3-8B throughput)."
fi

# ---------------------------------------------------------------------------
# Verify key imports
# ---------------------------------------------------------------------------
echo ""
echo "Verifying key imports..."
python - <<'EOF'
imports = [
    ("transformers", "AutoModelForCausalLM"),
    ("peft", "LoraConfig"),
    ("bitsandbytes", None),
    ("trl", "SFTTrainer"),
    ("accelerate", None),
    ("anyjev", "Tacit"),
]
for mod, attr in imports:
    try:
        m = __import__(mod)
        if attr:
            getattr(m, attr)
        print(f"  OK  {mod}")
    except Exception as e:
        print(f"  FAIL {mod}: {e}")
EOF

echo ""
echo "Setup complete."
echo "Activate with: source python/.venv/bin/activate"
echo "Train with:    uv run experiments/exp_001_qlora/train.py"
