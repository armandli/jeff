#!/usr/bin/env bash
# Set up the jeff Python environment for Qwen3-8B QLoRA training on an NVIDIA GPU.
#
# Usage:
#   bash scripts/setup_env.sh [--flash-attn] [--cuda 13.2] [--venv PATH] [--recreate]
#
# The script:
#   1. Creates (or reuses) a uv virtual-env at python/.venv
#   2. Detects the CUDA version the NVIDIA *driver* supports and installs the
#      newest PyTorch built for a CUDA version the driver can run
#   3. Installs the build backend (hatchling, editables) and all project deps
#   4. Optionally installs flash-attn (takes ~20 min if compiled from source)
#
# Options:
#   --cuda X.Y    Use this CUDA version instead of the detected driver version.
#                 The newest PyTorch index at or below it is still chosen.
#   --venv PATH   Install into PATH instead of python/.venv.
#   --recreate    Delete and recreate the venv instead of reusing it.
#
# After this script completes, activate with:
#   source python/.venv/bin/activate

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PYTHON_DIR="${REPO_ROOT}/python"
PYTHON_VERSION="3.11"
TORCH_WHL_ROOT="https://download.pytorch.org/whl"

INSTALL_FLASH_ATTN=false
CUDA_OVERRIDE=""
VENV="${PYTHON_DIR}/.venv"
RECREATE=false

# ---------------------------------------------------------------------------
# Parse args
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --flash-attn) INSTALL_FLASH_ATTN=true ;;
        --cuda)       CUDA_OVERRIDE="$2"; shift ;;
        --venv)       VENV="$(realpath -m "$2")"; shift ;;
        --recreate)   RECREATE=true ;;
        *) echo "Unknown argument: $1"; exit 1 ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Detect CUDA version
# ---------------------------------------------------------------------------
# PyTorch wheels bundle their own CUDA runtime, so what matters is the highest
# CUDA version the installed driver supports (nvidia-smi), not the local nvcc
# toolkit. Newer drivers print "CUDA UMD Version: X.Y"; older ones "CUDA Version: X.Y".
detect_cuda() {
    if [[ -n "${CUDA_OVERRIDE}" ]]; then
        echo "${CUDA_OVERRIDE}"
    elif command -v nvidia-smi &>/dev/null; then
        nvidia-smi | grep -oP 'CUDA (UMD )?Version: \K[0-9]+\.[0-9]+' | head -1 || true
    elif command -v nvcc &>/dev/null; then
        nvcc --version | grep -oP 'release \K[0-9]+\.[0-9]+' || true
    fi
}

CUDA_VERSION="$(detect_cuda)"

if [[ -z "${CUDA_VERSION}" ]]; then
    echo "ERROR: Could not detect CUDA. Is nvidia-smi or nvcc on PATH?"
    echo "       Pass --cuda <version> (e.g. --cuda 13.2) to override."
    exit 1
fi

# Tag "cu132" -> comparable integer 1302; version "13.3" -> 1303.
cuda_ver_num() { local maj="${1%%.*}" min="${1#*.}"; echo $(( maj * 100 + ${min%%.*} )); }
cuda_tag_num() { local d="${1#cu}"; echo $(( ${d:0:${#d}-1} * 100 + ${d: -1} )); }

DRIVER_NUM="$(cuda_ver_num "${CUDA_VERSION}")"
PY_TAG="cp${PYTHON_VERSION/./}"

echo "CUDA ${CUDA_VERSION} supported by driver; finding the newest PyTorch build for it..."

# Among the CUDA indexes PyTorch publishes that this driver can run, pick the
# one carrying the newest torch wheel for our Python (indexes lag each other:
# e.g. cu129 stopped at an older torch than cu132).
CUDA_TAG=""
TORCH_VERSION=""
TAGS="$(curl -fsSL "${TORCH_WHL_ROOT}/" | grep -oE '\bcu1[0-9]{2}\b' | sort -u || true)"
for tag in ${TAGS}; do
    (( $(cuda_tag_num "${tag}") <= DRIVER_NUM )) || continue
    (( $(cuda_tag_num "${tag}") >= 1206 )) || continue   # skip ancient indexes
    ver="$(curl -fsSL "${TORCH_WHL_ROOT}/${tag}/torch/" 2>/dev/null \
        | grep -oE "torch-[0-9]+\.[0-9]+\.[0-9]+(\+${tag})?-${PY_TAG}-${PY_TAG}-[^\"#<]*linux[^\"#<]*x86_64\.whl" \
        | sed -E 's/^torch-([0-9.]+).*/\1/' | sort -V | tail -1 || true)"
    [[ -n "${ver}" ]] || continue
    if [[ -z "${TORCH_VERSION}" ]] \
        || [[ "$(printf '%s\n%s\n' "${TORCH_VERSION}" "${ver}" | sort -V | tail -1)" == "${ver}" ]]; then
        TORCH_VERSION="${ver}"
        CUDA_TAG="${tag}"
    fi
done

if [[ -z "${CUDA_TAG}" ]]; then
    echo "ERROR: No PyTorch CUDA index found for CUDA <= ${CUDA_VERSION} and Python ${PYTHON_VERSION}."
    echo "       Check network access to ${TORCH_WHL_ROOT}/ or pass --cuda <version>."
    exit 1
fi

TORCH_INDEX="${TORCH_WHL_ROOT}/${CUDA_TAG}"
echo "Selected torch ${TORCH_VERSION} from ${TORCH_INDEX}"

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
if [[ -d "${VENV}" && "${RECREATE}" == "false" ]]; then
    echo "Reusing existing virtual environment at ${VENV} (pass --recreate for a fresh one)..."
else
    echo "Creating virtual environment at ${VENV}..."
    uv venv "${VENV}" --python "${PYTHON_VERSION}" --clear
fi

# Activate so subsequent uv pip calls use it
export VIRTUAL_ENV="${VENV}"
export PATH="${VENV}/bin:${PATH}"

# ---------------------------------------------------------------------------
# Install PyTorch with CUDA support
# ---------------------------------------------------------------------------
# torchvision/torchaudio are not used by jeff and are deliberately skipped:
# torchaudio's newest CUDA 13 wheels are pinned to an older torch and would
# drag torch back down.
echo "Installing torch ${TORCH_VERSION} (${CUDA_TAG})..."
uv pip install "torch==${TORCH_VERSION}" --index-url "${TORCH_INDEX}"

# ---------------------------------------------------------------------------
# Install project dependencies
# ---------------------------------------------------------------------------
# The editable install uses --no-build-isolation (so it builds against the
# CUDA torch above), which means the build backend must already be in the venv.
echo "Installing build backend (hatchling, editables)..."
uv pip install hatchling editables

echo "Installing jeff project dependencies..."
cd "${PYTHON_DIR}"
TORCH_BEFORE="$(python -W ignore -c 'import torch; print(torch.__version__)')"
uv pip install -e ".[dev]" --no-build-isolation
TORCH_AFTER="$(python -W ignore -c 'import torch; print(torch.__version__)')"
if [[ "${TORCH_BEFORE}" != "${TORCH_AFTER}" ]]; then
    echo "ERROR: installing project deps replaced torch ${TORCH_BEFORE} with ${TORCH_AFTER}."
    echo "       A dependency probably pins an incompatible torch; check its requirements."
    exit 1
fi

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
echo "Activate with: source ${VENV}/bin/activate"
echo "Train with:    uv run experiments/exp_001_qlora/train.py"
