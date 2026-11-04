"""QLoRA trainer for Qwen3-8B + DecisionHead.

Training stages:
  1. SFT  — cross-entropy over per-option scores (letter log-prob + linear head)
  2. RLCD — DPO on (chosen, rejected) option pairs using the same head (TODO)

The head trains in fp32; LoRA adapters sit on a 4-bit NF4 base (PEFT + bitsandbytes).
Each training example is shown with a random option order (debiasing augmentation).

Runs are resumable: every `save_every` optimizer steps the adapter, head,
optimizer, scheduler and step counter are written to <output_dir>/latest, and a
restart picks up from there (the data order is a pure function of seed+epoch,
so already-seen micro-batches are skipped).
"""

import json
import math
import random
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

import torch
import torch.nn.functional as F

from jeff.models.base import DecisionInput
from jeff.models.qwen3_decision import Qwen3Config, Qwen3DecisionModel, encode_example


@dataclass
class QLoRATrainingConfig:
    # Model
    model_name: str = "Qwen/Qwen3-8B"
    load_in_4bit: bool = True
    lora_r: int = 64
    lora_alpha: int = 128
    lora_dropout: float = 0.05
    lora_target_modules: list[str] = field(default_factory=lambda: [
        "q_proj", "k_proj", "v_proj", "o_proj",
        "gate_proj", "up_proj", "down_proj",
    ])
    max_seq_len: int = 1024              # longer prompts are dropped from training

    # Training
    output_dir: str = "checkpoints/exp_001_qlora"
    num_epochs: int = 1
    max_steps: int | None = None         # optimizer-step cap (None = full epochs)
    batch_size: int = 4
    gradient_accumulation_steps: int = 8
    learning_rate: float = 2e-4
    head_learning_rate: float = 1e-3
    warmup_ratio: float = 0.05
    max_grad_norm: float = 1.0
    bf16: bool = True
    seed: int = 0

    # Eval / logging
    eval_every: int = 250                # optimizer steps; uses eval_subset examples
    eval_subset: int = 300
    log_every: int = 10
    save_every: int = 100
    wandb_project: str = "jeff"
    wandb_run: str = "exp_001_qlora"
    results_dir: str | None = None


class QLoRATrainer:
    """SFT (and later RLCD/DPO) on the linear head + LoRA."""

    def __init__(self, cfg: QLoRATrainingConfig):
        self.cfg = cfg

    def build_model(self) -> Qwen3DecisionModel:
        c = self.cfg
        model = Qwen3DecisionModel(Qwen3Config(
            model_name=c.model_name, load_in_4bit=c.load_in_4bit,
            lora_r=c.lora_r, lora_alpha=c.lora_alpha, lora_dropout=c.lora_dropout,
            lora_target_modules=c.lora_target_modules, max_seq_len=c.max_seq_len,
        ))
        model.load_base_for_training()
        return model

    # ------------------------------------------------------------------

    def _encode_train(self, model, dataset) -> list[dict]:
        rng = random.Random(self.cfg.seed)
        items, dropped = [], 0
        for ex in dataset:
            perm = list(range(len(ex.options)))
            rng.shuffle(perm)
            it = encode_example(model.tokenizer, ex.context, ex.options, perm)
            if len(it["input_ids"]) > self.cfg.max_seq_len:
                dropped += 1
                continue
            it["label"] = perm.index(ex.correct_index)   # shown letter of the answer
            items.append(it)
        print(f"train examples: {len(items)} kept, {dropped} dropped (> {self.cfg.max_seq_len} tokens)",
              flush=True)
        return items

    def evaluate(self, model, examples, tag: str) -> dict:
        model.backbone.eval()
        preds, preds_letter, labels = [], [], []
        nll = 0.0
        t0 = time.time()
        for ex in examples:
            s, s_letter = model.score_options_both(DecisionInput(ex.context, ex.options))
            preds.append(max(range(len(s)), key=s.__getitem__))
            preds_letter.append(max(range(len(s_letter)), key=s_letter.__getitem__))
            labels.append(ex.correct_index)
            nll -= s[ex.correct_index]
        model.backbone.train()
        n = len(labels)
        res = {
            "tag": tag, "n": n,
            "accuracy": sum(p == l for p, l in zip(preds, labels)) / n,
            "accuracy_letter_only": sum(p == l for p, l in zip(preds_letter, labels)) / n,
            "nll": nll / n,
            "pred_dist": {k: preds.count(k) for k in sorted(set(preds))},
            "seconds": round(time.time() - t0, 1),
        }
        print("EVAL " + json.dumps(res), flush=True)
        return res

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

        lora_params = [p for _, p in model.backbone.named_parameters() if p.requires_grad]
        head_params = list(model.head.parameters())
        optimizer = AdamW(
            [{"params": lora_params, "lr": c.learning_rate},
             {"params": head_params, "lr": c.head_learning_rate}],
            weight_decay=0.01,
        )
        micro_per_epoch = math.ceil(len(items) / c.batch_size)
        steps_per_epoch = micro_per_epoch // c.gradient_accumulation_steps
        total_steps = steps_per_epoch * c.num_epochs
        if c.max_steps:
            total_steps = min(total_steps, c.max_steps)
        scheduler = get_cosine_schedule_with_warmup(
            optimizer, int(total_steps * c.warmup_ratio), total_steps)

        # ---- resume -------------------------------------------------------
        step = 0
        latest = out / "latest"
        history: list[dict] = []
        if (latest / "trainer_state.pt").exists():
            from peft import set_peft_model_state_dict
            from safetensors.torch import load_file
            set_peft_model_state_dict(model.backbone, load_file(str(latest / "adapter_model.safetensors")))
            model.head.load_state_dict(torch.load(latest / "decision_head.pt"))
            st = torch.load(latest / "trainer_state.pt", weights_only=False)
            optimizer.load_state_dict(st["optimizer"])
            scheduler.load_state_dict(st["scheduler"])
            step = st["step"]
            history = st.get("history", [])
            print(f"resumed from {latest} at step {step}", flush=True)

        wandb.init(project=c.wandb_project, name=c.wandb_run, config=asdict(c),
                   mode="offline", dir=str(out))

        def log_eval(res):
            history.append(res)
            prev = [h["accuracy"] for h in history[:-1] if h["tag"].startswith("step")]
            if res["tag"].startswith("step") and (not prev or res["accuracy"] > max(prev)):
                model.save_pretrained(str(out / "best"))
                (out / "best" / "eval.json").write_text(json.dumps(res, indent=2) + "\n")
                print(f"new best {res['accuracy']:.4f} at step {res['step']} → {out / 'best'}", flush=True)
            (results_dir / "eval_history.json").write_text(json.dumps(history, indent=2) + "\n")
            wandb.log({f"eval/{k}": v for k, v in res.items() if isinstance(v, float)}, step=step)

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
            if first_micro >= steps_per_epoch * c.gradient_accumulation_steps:
                continue
            for mi in range(first_micro, steps_per_epoch * c.gradient_accumulation_steps):
                batch = model.collate([items[i] for i in order[mi * c.batch_size:(mi + 1) * c.batch_size]])
                batch = {k: v.cuda() for k, v in batch.items()}
                labels = batch.pop("labels")
                with torch.autocast("cuda", dtype=torch.bfloat16, enabled=c.bf16):
                    scores, _ = model.forward_for_training(**batch)
                loss = F.cross_entropy(scores.float(), labels)
                (loss / c.gradient_accumulation_steps).backward()
                run_loss += loss.item()
                run_acc += (scores.argmax(-1) == labels).float().mean().item()
                run_n += 1

                if (mi + 1) % c.gradient_accumulation_steps:
                    continue
                gn = torch.nn.utils.clip_grad_norm_(lora_params + head_params, c.max_grad_norm)
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
                if step >= total_steps:
                    done = True
                    break

        model.save_pretrained(str(out / "final"))
        print(f"Saved final → {out / 'final'}", flush=True)
        if eval_sub:
            log_eval({**self.evaluate(model, eval_sub, "final_subset"), "step": step})
        if eval_list:
            res = {**self.evaluate(model, eval_list, "final_full_val"), "step": step}
            log_eval(res)
            (results_dir / "final_val.json").write_text(json.dumps(res, indent=2) + "\n")
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
