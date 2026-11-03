"""Export a trained jeff checkpoint to TorchScript for LibTorch C++ inference.

Usage:
    uv run scripts/export_model.py --checkpoint checkpoints/exp_000_baseline --out models/jeff.pt
"""

import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--checkpoint", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    import sys
    sys.path.insert(0, str(Path(__file__).parent.parent / "python"))

    from jeff.models.qwen_decision import QwenDecisionModel

    print(f"Loading from {args.checkpoint}...")
    model = QwenDecisionModel(args.checkpoint, device="cpu")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    model.export_torchscript(str(out))
    print(f"Exported TorchScript model to {out}")


if __name__ == "__main__":
    main()
