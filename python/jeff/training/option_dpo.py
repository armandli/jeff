"""DPO on per-option scores (RLCD stage) for the Qwen3 QLoRA + linear-head model.

Each training item is (context, options, chosen, rejected). The model's option
scores s (letter log-prob + head, as in SFT) play the role of the policy log-probs:

    margin   = (s[chosen] - s[rejected]) - (s_ref[chosen] - s_ref[rejected])
    loss     = -log sigmoid(beta * margin)  +  nll_weight * CE(s, chosen)

The reference is the starting checkpoint (normally the SFT adapter); its margins
are precomputed once with the same option permutation the item is trained with,
so no second model is kept in memory. The NLL term (RPO-style) keeps the chosen
option's probability from collapsing while margins grow.
"""

from __future__ import annotations

import json
import math
import random
import time
from dataclasses import asdict, dataclass
from pathlib import Path

import torch
import torch.nn.functional as F

from jeff.models.qwen3_decision import encode_example
from jeff.training.qlora_trainer import QLoRATrainer, QLoRATrainingConfig


@dataclass
class PrefPair:
    context: str
    options: list[str]
    chosen: int
    rejected: int


def load_pairs(path: str | Path) -> list[PrefPair]:
    out = []
    for line in open(path):
        if line.strip():
            d = json.loads(line)
            out.append(PrefPair(d["context"], d["options"], int(d["chosen"]), int(d["rejected"])))
    return out


@dataclass
class OptionDPOConfig(QLoRATrainingConfig):
    init_adapter: str | None = None      # SFT checkpoint dir (adapter + decision_head.pt)
    beta: float = 1.0
    nll_weight: float = 0.2


class OptionDPOTrainer(QLoRATrainer):
    cfg: OptionDPOConfig

    def _load_init(self, model) -> None:
        from peft import set_peft_model_state_dict
        from safetensors.torch import load_file

        d = Path(self.cfg.init_adapter)
        res = set_peft_model_state_dict(model.backbone, load_file(str(d / "adapter_model.safetensors")))
        unexpected = getattr(res, "unexpected_keys", [])
        assert not unexpected, f"adapter keys did not match: {unexpected[:3]}"
        model.head.load_state_dict(torch.load(d / "decision_head.pt"))
        print(f"initialised from {d}", flush=True)

    def _encode(self, model, pairs: list[PrefPair], seed: int) -> list[dict]:
        rng = random.Random(seed)
        items = []
        for p in pairs:
            perm = list(range(len(p.options)))
            rng.shuffle(perm)
            it = encode_example(model.tokenizer, p.context, p.options, perm)
            if len(it["input_ids"]) > self.cfg.max_seq_len:
                continue
            it["chosen"], it["rejected"] = perm.index(p.chosen), perm.index(p.rejected)
            items.append(it)
        return items

    @torch.no_grad()
    def _margins(self, model, items: list[dict], bs: int = 16) -> list[float]:
        model.backbone.eval()
        out = []
        for i in range(0, len(items), bs):
            chunk = items[i:i + bs]
            batch = {k: v.cuda() for k, v in model.collate(chunk).items()}
            with torch.autocast("cuda", dtype=torch.bfloat16, enabled=self.cfg.bf16):
                s, _ = model.forward_for_training(**batch)
            s = s.float()
            c = torch.tensor([it["chosen"] for it in chunk], device=s.device)
            r = torch.tensor([it["rejected"] for it in chunk], device=s.device)
            out += (s.gather(1, c[:, None]) - s.gather(1, r[:, None])).squeeze(1).tolist()
        model.backbone.train()
        return out

    def _pref_eval(self, model, items, ref, tag, step) -> dict:
        t0 = time.time()
        m = self._margins(model, items)
        res = {"tag": tag, "step": step, "n": len(m),
               "pref_acc": sum(x > 0 for x in m) / len(m),
               "ref_pref_acc": sum(x > 0 for x in ref) / len(ref),
               "margin_mean": sum(m) / len(m),
               "dpo_margin_mean": sum(a - b for a, b in zip(m, ref)) / len(m),
               "seconds": round(time.time() - t0, 1)}
        print("PREF " + json.dumps(res), flush=True)
        return res

    def train_dpo(self, train_pairs: list[PrefPair], eval_pairs: list[PrefPair],
                  state_eval=None) -> None:
        import wandb
        from torch.optim import AdamW
        from transformers import get_cosine_schedule_with_warmup

        c = self.cfg
        out = Path(c.output_dir)
        out.mkdir(parents=True, exist_ok=True)
        torch.manual_seed(c.seed)
        model = self.build_model()
        if c.init_adapter:
            self._load_init(model)

        items = self._encode(model, train_pairs, c.seed)
        ev_items = self._encode(model, eval_pairs, c.seed + 1)
        state_sub = (random.Random(1).sample(list(state_eval), min(c.eval_subset, len(state_eval)))
                     if state_eval is not None else [])
        print(f"pairs: train {len(items)}, eval {len(ev_items)}", flush=True)

        ref_path = out / "ref_margins.pt"
        if ref_path.exists():
            ref = torch.load(ref_path)
        else:
            t0 = time.time()
            ref = {"train": self._margins(model, items), "eval": self._margins(model, ev_items)}
            torch.save(ref, ref_path)
            print(f"reference margins computed in {time.time() - t0:.0f}s", flush=True)
        for it, r in zip(items, ref["train"]):
            it["ref"] = r

        lora_params = [p for _, p in model.backbone.named_parameters() if p.requires_grad]
        head_params = list(model.head.parameters())
        optimizer = AdamW([{"params": lora_params, "lr": c.learning_rate},
                           {"params": head_params, "lr": c.head_learning_rate}], weight_decay=0.01)
        micro_per_epoch = math.ceil(len(items) / c.batch_size)
        steps_per_epoch = micro_per_epoch // c.gradient_accumulation_steps
        total_steps = steps_per_epoch * c.num_epochs
        if c.max_steps:
            total_steps = min(total_steps, c.max_steps)
        scheduler = get_cosine_schedule_with_warmup(optimizer, int(total_steps * c.warmup_ratio), total_steps)

        step, history = 0, []
        latest = out / "latest"
        if (latest / "trainer_state.pt").exists():
            from peft import set_peft_model_state_dict
            from safetensors.torch import load_file
            set_peft_model_state_dict(model.backbone, load_file(str(latest / "adapter_model.safetensors")))
            model.head.load_state_dict(torch.load(latest / "decision_head.pt"))
            st = torch.load(latest / "trainer_state.pt", weights_only=False)
            optimizer.load_state_dict(st["optimizer"])
            scheduler.load_state_dict(st["scheduler"])
            step, history = st["step"], st.get("history", [])
            print(f"resumed from {latest} at step {step}", flush=True)

        wandb.init(project=c.wandb_project, name=c.wandb_run, config=asdict(c), mode="offline", dir=str(out))

        def do_eval(tag):
            res = self._pref_eval(model, ev_items, ref["eval"], tag, step)
            if state_sub:
                s = self.evaluate(model, state_sub, tag + "_teacher_states")
                res["teacher_state_acc"] = s["accuracy"]
            history.append(res)
            (out / "eval_history.json").write_text(json.dumps(history, indent=2) + "\n")
            # Checkpoints are kept per eval step; the in-game score picks the winner,
            # held-out preference accuracy is only a guide.
            model.save_pretrained(str(out / f"step{step}"))
            wandb.log({f"eval/{k}": v for k, v in res.items() if isinstance(v, float)}, step=step)

        if step == 0:
            do_eval("step0")
        print(f"total optimizer steps: {total_steps} ({steps_per_epoch}/epoch)", flush=True)
        model.backbone.train()
        t_start, agg, run_n = time.time(), {"loss": 0.0, "dpo": 0.0, "nll": 0.0, "acc": 0.0}, 0
        done = step >= total_steps
        for epoch in range(c.num_epochs):
            if done:
                break
            order = list(range(len(items)))
            random.Random(c.seed * 1000 + epoch).shuffle(order)
            first = max(0, step * c.gradient_accumulation_steps - epoch * micro_per_epoch)
            for mi in range(first, steps_per_epoch * c.gradient_accumulation_steps):
                chunk = [items[i] for i in order[mi * c.batch_size:(mi + 1) * c.batch_size]]
                batch = {k: v.cuda() for k, v in model.collate(chunk).items()}
                ch = torch.tensor([it["chosen"] for it in chunk], device="cuda")
                rj = torch.tensor([it["rejected"] for it in chunk], device="cuda")
                rf = torch.tensor([it["ref"] for it in chunk], device="cuda")
                with torch.autocast("cuda", dtype=torch.bfloat16, enabled=c.bf16):
                    s, _ = model.forward_for_training(**batch)
                s = s.float()
                margin = s.gather(1, ch[:, None]).squeeze(1) - s.gather(1, rj[:, None]).squeeze(1)
                dpo = -F.logsigmoid(c.beta * (margin - rf)).mean()
                nll = F.cross_entropy(s, ch)
                loss = dpo + c.nll_weight * nll
                (loss / c.gradient_accumulation_steps).backward()
                agg["loss"] += loss.item(); agg["dpo"] += dpo.item(); agg["nll"] += nll.item()
                agg["acc"] += (margin > 0).float().mean().item()
                run_n += 1
                if (mi + 1) % c.gradient_accumulation_steps:
                    continue
                gn = torch.nn.utils.clip_grad_norm_(lora_params + head_params, c.max_grad_norm)
                optimizer.step(); scheduler.step(); optimizer.zero_grad(set_to_none=True)
                step += 1
                if step % c.log_every == 0:
                    rec = {"step": step, "total": total_steps, **{k: v / run_n for k, v in agg.items()},
                           "grad_norm": float(gn), "lr": scheduler.get_last_lr()[0],
                           "elapsed_min": round((time.time() - t_start) / 60, 1)}
                    print("TRAIN " + json.dumps(rec), flush=True)
                    wandb.log({f"train/{k}": v for k, v in rec.items()}, step=step)
                    agg, run_n = {k: 0.0 for k in agg}, 0
                if step % c.save_every == 0 or step >= total_steps:
                    self._save_state(model, optimizer, scheduler, step, history, latest)
                if step % c.eval_every == 0 or step >= total_steps:
                    do_eval(f"step{step}")
                if step >= total_steps:
                    done = True
                    break
        model.save_pretrained(str(out / "final"))
        print(f"Saved final → {out / 'final'}", flush=True)
        wandb.finish()
