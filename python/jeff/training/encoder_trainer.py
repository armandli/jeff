"""Trainer for ModernBERT encoder decision model (exp_003).

Same patterns as qlora_trainer but no bitsandbytes / quantisation:
  - LoRA on encoder attention (or full fine-tune)
  - Separate LR for backbone vs head
  - Resume from checkpoints/exp_003_fast_encoder/latest
  - Periodic eval on a subset; best checkpoint saved to <dir>/best
"""

import json
import math
import random
import time
from collections.abc import Callable
from dataclasses import asdict, dataclass, field
from pathlib import Path

import torch
import torch.nn.functional as F

from jeff.models.base import DecisionInput
from jeff.models.modernbert_decision import ModernBERTConfig, ModernBERTDecisionModel


@dataclass
class EncoderTrainingConfig:
    # Model
    model_name: str = "answerdotai/ModernBERT-large"
    full_finetune: bool = False
    lora_r: int = 16
    lora_alpha: int = 32
    lora_dropout: float = 0.05
    lora_target_modules: list[str] = field(default_factory=lambda: [
        "Wqkv", "o_proj", "Wi", "Wo",
    ])
    max_seq_len: int = 512
    n_permutations: int = 0            # ignored by ModernBERTDecisionModel (order-independent)

    # Training
    output_dir: str = "checkpoints/exp_003_fast_encoder"
    num_epochs: int = 3
    max_steps: int | None = None
    batch_size: int = 8                # higher than QLoRA: smaller model, more VRAM headroom
    gradient_accumulation_steps: int = 4   # effective batch = 32
    learning_rate: float = 2e-5        # encoder backbone
    head_learning_rate: float = 1e-4   # linear decision head
    warmup_ratio: float = 0.05
    max_grad_norm: float = 1.0
    bf16: bool = True
    seed: int = 0
    # std of a random normal init for the decision head; 0 keeps the zero init (untrained
    # model scores every option equally). With a zero head no gradient reaches the backbone
    # until the head itself has grown, and RoBERTa can then sit at loss ln2 for a whole run.
    head_init_std: float = 0.0
    pooling: str = "cls"               # see ModernBERTConfig.pooling

    # Eval / logging
    eval_every: int = 500
    eval_subset: int = 300
    speed_eval_every: int = 0            # run speed_benchmark every N steps; 0 = disabled
    log_every: int = 10
    save_every: int = 100
    wandb_project: str = "jeff"
    wandb_run: str = "exp_003_fast_encoder"
    results_dir: str | None = None


# rank_fn(examples, probs) -> dict of extra eval metrics, where probs[i] is the list of
# option probabilities for examples[i]. If it returns "rank_score", checkpoints are
# ranked on that (higher is better) instead of plain argmax accuracy.
RankFn = Callable[[list, list[list[float]]], dict]


class EncoderTrainer:

    def __init__(self, cfg: EncoderTrainingConfig, rank_fn: RankFn | None = None):
        self.cfg = cfg
        self.rank_fn = rank_fn

    def build_model(self) -> ModernBERTDecisionModel:
        c = self.cfg
        model = ModernBERTDecisionModel(ModernBERTConfig(
            model_name=c.model_name,
            full_finetune=c.full_finetune,
            lora_r=c.lora_r,
            lora_alpha=c.lora_alpha,
            lora_dropout=c.lora_dropout,
            lora_target_modules=c.lora_target_modules,
            max_seq_len=c.max_seq_len,
            n_permutations=c.n_permutations,
            pooling=c.pooling,
        ))
        model.load_base_for_training()
        if c.head_init_std > 0:
            torch.nn.init.normal_(model.head.proj.weight, std=c.head_init_std)
        return model

    # ------------------------------------------------------------------

    def _encode_train(self, model: ModernBERTDecisionModel, dataset) -> list[dict]:
        rng = random.Random(self.cfg.seed)
        items, dropped = [], 0
        for ex in dataset:
            perm = list(range(len(ex.options)))
            rng.shuffle(perm)
            enc = model.encode_example(ex.context, ex.options, perm, truncate=False)
            if max(len(x) for x in enc["input_ids"]) > self.cfg.max_seq_len:
                dropped += 1
                continue
            enc["label"] = perm.index(ex.correct_index)
            items.append(enc)
        print(f"train examples: {len(items)} kept, {dropped} dropped (> {self.cfg.max_seq_len} tok)",
              flush=True)
        return items

    def evaluate(self, model: ModernBERTDecisionModel, examples, tag: str) -> dict:
        model.backbone.eval()
        preds, labels, probs = [], [], []
        nll = 0.0
        t0 = time.time()
        for ex in examples:
            inp = DecisionInput(ex.context, ex.options)
            s = model.score_options(inp)
            preds.append(max(range(len(s)), key=s.__getitem__))
            labels.append(ex.correct_index)
            probs.append([math.exp(x) for x in s])
            nll -= s[ex.correct_index]
        model.backbone.train()
        n = len(labels)
        res = {
            "tag": tag, "n": n,
            "accuracy": sum(p == l for p, l in zip(preds, labels)) / n,
            "nll": nll / n,
            "pred_dist": {k: preds.count(k) for k in sorted(set(preds))},
            "seconds": round(time.time() - t0, 1),
            "ms_per_decision": round((time.time() - t0) * 1000 / n, 2),
        }
        if self.rank_fn:
            res.update(self.rank_fn(list(examples), probs))
        print("EVAL " + json.dumps(res), flush=True)
        return res

    def speed_benchmark(
        self, model: ModernBERTDecisionModel, examples, batch_size: int = 32
    ) -> dict:
        """Measure single-decision and batched throughput."""
        import statistics

        device = model.head.proj.weight.device
        model.backbone.eval()
        subset = examples[:200]

        # Warmup
        for ex in subset[:10]:
            model.score_options(DecisionInput(ex.context, ex.options))

        # Sequential single-decision latency
        seq_times = []
        for ex in subset:
            t0 = time.perf_counter()
            model.score_options(DecisionInput(ex.context, ex.options))
            seq_times.append((time.perf_counter() - t0) * 1000)

        # Batched throughput: collate `batch_size` examples per forward pass
        batches = [subset[i:i+batch_size] for i in range(0, len(subset), batch_size)]
        batch_times = []
        for batch in batches:
            n = len(batch)
            # Encode all with identity permutation
            items = [model.encode_example(ex.context, ex.options, list(range(len(ex.options))))
                     for ex in batch]
            col = model.collate(items)
            ids = col["input_ids"].to(device)
            am  = col["attention_mask"].to(device)
            n_opts = col["n_options"]
            t0 = time.perf_counter()
            with torch.no_grad():
                model.forward_for_training(ids, am, n_opts)
            torch.cuda.synchronize(device) if device.type == "cuda" else None
            batch_times.append((time.perf_counter() - t0) * 1000 / n)

        model.backbone.train()
        return {
            "sequential_ms_p50": round(statistics.median(seq_times), 2),
            "sequential_ms_p95": round(sorted(seq_times)[int(0.95 * len(seq_times))], 2),
            "sequential_decisions_per_sec": round(1000 / statistics.median(seq_times), 1),
            "batch_ms_per_decision_p50": round(statistics.median(batch_times), 2),
            "batch_decisions_per_sec": round(1000 / statistics.median(batch_times), 1),
            "batch_size": batch_size,
            "device": str(device),
            # Reference numbers from exp_001 (Qwen3-8B QLoRA on same MMLU val set)
            "exp001_sequential_ms_per_decision": 202.2,
        }

    # ------------------------------------------------------------------

    def train_sft(self, train_dataset, eval_dataset=None) -> None:
        import wandb
        from torch.optim import AdamW
        from transformers import get_cosine_schedule_with_warmup

        c = self.cfg
        out = Path(c.output_dir)
        out.mkdir(parents=True, exist_ok=True)
        results_dir = Path(c.results_dir) if c.results_dir else out
        results_dir.mkdir(parents=True, exist_ok=True)
        torch.manual_seed(c.seed)

        model = self.build_model()
        items = self._encode_train(model, train_dataset)
        eval_list = list(eval_dataset) if eval_dataset is not None else []
        eval_sub = random.Random(1).sample(eval_list, min(c.eval_subset, len(eval_list)))

        backbone_params = [p for _, p in model.backbone.named_parameters() if p.requires_grad]
        head_params = list(model.head.parameters())
        optimizer = AdamW(
            [{"params": backbone_params, "lr": c.learning_rate},
             {"params": head_params, "lr": c.head_learning_rate}],
            weight_decay=0.01,
        )
        micro_per_epoch = math.ceil(len(items) / c.batch_size)
        steps_per_epoch = micro_per_epoch // c.gradient_accumulation_steps
        total_steps = steps_per_epoch * c.num_epochs
        if c.max_steps:
            total_steps = min(total_steps, c.max_steps)
        scheduler = get_cosine_schedule_with_warmup(
            optimizer, int(total_steps * c.warmup_ratio), total_steps
        )

        # ---- resume -------------------------------------------------------
        step = 0
        latest = out / "latest"
        history: list[dict] = []
        if (latest / "trainer_state.pt").exists():
            if c.full_finetune:
                model.backbone.load_state_dict(
                    torch.load(latest / "pytorch_model.bin", weights_only=True)
                )
            else:
                from peft import set_peft_model_state_dict
                from safetensors.torch import load_file
                set_peft_model_state_dict(
                    model.backbone,
                    load_file(str(latest / "adapter_model.safetensors"))
                )
            model.head.load_state_dict(torch.load(latest / "decision_head.pt"))
            st = torch.load(latest / "trainer_state.pt", weights_only=False)
            optimizer.load_state_dict(st["optimizer"])
            scheduler.load_state_dict(st["scheduler"])
            step = st["step"]
            history = st.get("history", [])
            print(f"resumed from {latest} at step {step}", flush=True)

        wandb.init(project=c.wandb_project, name=c.wandb_run, config=asdict(c),
                   mode="offline", dir=str(out))

        def rank(h):
            return h.get("rank_score", h["accuracy"])

        def log_eval(res):
            history.append(res)
            prev = [rank(h) for h in history[:-1] if h["tag"].startswith("step")]
            if res["tag"].startswith("step") and (not prev or rank(res) > max(prev)):
                model.save_pretrained(str(out / "best"))
                (out / "best" / "eval.json").write_text(json.dumps(res, indent=2) + "\n")
                print(f"new best {rank(res):.4f} @ step {res.get('step', '?')} → {out/'best'}",
                      flush=True)
            (results_dir / "eval_history.json").write_text(json.dumps(history, indent=2) + "\n")
            wandb.log({f"eval/{k}": v for k, v in res.items() if isinstance(v, float)},
                      step=res.get("step", step))

        if step == 0 and eval_sub:
            log_eval({**self.evaluate(model, eval_sub, "zeroshot_subset"), "step": 0})

        print(f"total optimizer steps: {total_steps} ({steps_per_epoch}/epoch)", flush=True)
        model.backbone.train()
        t_start, run_loss, run_acc, run_n = time.time(), 0.0, 0.0, 0
        done = step >= total_steps

        for epoch in range(c.num_epochs):
            if done:
                break
            order = list(range(len(items)))
            random.Random(c.seed * 1000 + epoch).shuffle(order)
            first_micro = max(0, step * c.gradient_accumulation_steps - epoch * micro_per_epoch)
            for mi in range(first_micro, micro_per_epoch):
                batch_items = [items[order[j]]
                               for j in range(mi * c.batch_size, (mi + 1) * c.batch_size)
                               if j < len(order)]
                if not batch_items:
                    continue
                batch = model.collate(batch_items)
                dev = model.head.proj.weight.device
                labels = batch.pop("labels").to(dev)
                n_opts = batch.pop("n_options")
                batch.pop("counts", None)
                batch = {k: v.to(dev) for k, v in batch.items()}

                with torch.autocast("cuda", dtype=torch.bfloat16, enabled=c.bf16):
                    scores = model.forward_for_training(
                        batch["input_ids"], batch["attention_mask"], n_opts
                    )
                loss = F.cross_entropy(scores.float(), labels)
                (loss / c.gradient_accumulation_steps).backward()
                run_loss += loss.item()
                run_acc += (scores.argmax(-1) == labels).float().mean().item()
                run_n += 1

                if (mi + 1) % c.gradient_accumulation_steps:
                    continue
                gn = torch.nn.utils.clip_grad_norm_(
                    backbone_params + head_params, c.max_grad_norm
                )
                optimizer.step()
                scheduler.step()
                optimizer.zero_grad(set_to_none=True)
                step += 1

                if step % c.log_every == 0:
                    el = time.time() - t_start
                    rec = {"step": step, "total": total_steps, "loss": run_loss / run_n,
                           "train_acc": run_acc / run_n, "grad_norm": float(gn),
                           "lr": scheduler.get_last_lr()[0], "elapsed_min": round(el / 60, 1)}
                    print("TRAIN " + json.dumps(rec), flush=True)
                    wandb.log({f"train/{k}": v for k, v in rec.items()}, step=step)
                    run_loss, run_acc, run_n = 0.0, 0.0, 0
                if step % c.save_every == 0 or step >= total_steps:
                    self._save_state(model, optimizer, scheduler, step, history, latest)
                if eval_sub and step % c.eval_every == 0 and step < total_steps:
                    log_eval({**self.evaluate(model, eval_sub, f"step{step}_subset"), "step": step})
                if c.speed_eval_every and step % c.speed_eval_every == 0 and eval_list:
                    speed = self.speed_benchmark(model, eval_list)
                    print("SPEED " + json.dumps({**speed, "step": step}), flush=True)
                    wandb.log({f"speed/{k}": v for k, v in speed.items()
                               if isinstance(v, (int, float))}, step=step)
                if step >= total_steps:
                    done = True
                    break

        model.save_pretrained(str(out / "final"))
        print(f"Saved final → {out / 'final'}", flush=True)
        if eval_list:
            res = {**self.evaluate(model, eval_list, "final_full_val"), "step": step}
            log_eval(res)
            speed = self.speed_benchmark(model, eval_list)
            res_full = {**res, "speed": speed}
            (results_dir / "final_val.json").write_text(json.dumps(res_full, indent=2) + "\n")
            print("SPEED " + json.dumps(speed), flush=True)
        wandb.finish()

    def _save_state(self, model, optimizer, scheduler, step, history, latest: Path) -> None:
        tmp = latest.with_name("latest.tmp")
        model.save_pretrained(str(tmp))
        torch.save({"optimizer": optimizer.state_dict(), "scheduler": scheduler.state_dict(),
                    "step": step, "history": history}, tmp / "trainer_state.pt")
        if latest.exists():
            import shutil
            shutil.rmtree(latest)
        tmp.rename(latest)
        print(f"checkpoint step {step} → {latest}", flush=True)
