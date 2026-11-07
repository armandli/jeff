# exp_004 results — GLiNER2 action classifier on Doom (obs v2)

Run 2026-11-07: fastino/gliner2.5-base-v1 (194M), LoRA r16 (2.7M trainable params), task_lr 5e-4,
batch size 16, 3 epochs (4,500 steps, ~10 min on an RTX 5080 Laptop GPU). Data: doom_v2_sft (24k states labelled by the v2 teacher).
`best` = lowest eval_loss (step 4000).

| model | val agreement | seed 7 x20 kills | seed 11 x50 kills | ms/decision |
|---|---|---|---|---|
| GLiNER2, no fine-tuning | 0.21 | – | – | 13 |
| GLiNER2 LoRA, 30 steps (smoke test) | 0.83 | 0.0 (2 ep, never fires) | – | 14 |
| **GLiNER2 LoRA best** | – | **9.60 ± 1.80** | **9.32 ± 1.82** | **13** |
| GLiNER2 LoRA final | 0.974 | 9.35 ± 1.88 | – | 13 |
| teacher v2 (exp_002) | 1.0 | 9.20 | 9.14 ± 1.73 | ~0 |
| Qwen3-8B v2 SFT (exp_002) | ~0.98 | 9.00 | 8.98 ± 1.76 | 319 |

GLiNER2 matches the teacher it imitates (the differences are within ~1 SE, ≈0.25 at n=50) at about 25x
lower latency than Qwen3-8B. Like Qwen SFT, it doesn't exceed the teacher; imitation caps out at the teacher's level.
