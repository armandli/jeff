# exp_001 results — Qwen3-8B QLoRA + linear decision head on MMLU

MMLU val, 1531 questions, all 4 cyclic option rotations averaged.

| model | accuracy | letter-only acc | NLL |
|---|---|---|---|
| zero-shot Qwen3-8B (4-bit) | 71.6% | 71.6% | 2.76 |
| QLoRA step 400 (`checkpoints/exp_001_qlora/best`) | **74.7%** | 74.6% | 0.70 |

Run (2026-11-04): LoRA r64 on all projections, LoRA lr 5e-5, head lr 1e-3, effective batch 32,
trained on `data/processed/mmlu_train.jsonl` (auxiliary_train). Stopped at step ~650 of the 3113-step
epoch to save time; the checkpoint kept is the best by 300-question val subset (step 400: 77.3%),
so the full-val number carries a little selection bias (the subset is part of val). Per-step
subset evals are in `eval_history.json`.

Notes
- A first attempt at lr 2e-4 was unstable: subset accuracy reached ~73% at step 200, then fell
  to chance (27%) by step 250.
- The linear head contributes almost nothing on top of the LoRA-tuned letter logits (+0.1 pt);
  the gain comes from the adapter. NLL improves 4x, i.e. calibration got much better.
- MMLU test has not been touched.
