"""Experiment 002 — play ViZDoom defend_the_center with a text-only brain.

Brains:
  random     uniform over the three buttons (floor)
  heuristic  scripted aim-bot teacher (bar to beat)
  qwen3      Qwen3-8B zero-shot (or --adapter LoRA) via option-letter scoring

Every run writes a results JSON; --record also writes one JSONL line per
decision in jeff's DecisionExample format (context / options / correct_index,
where correct_index is the TEACHER's action), so recordings from any brain
double as SFT data. Extra fields (taken action, reward, scores) ride along and
are ignored by the loader.

Examples (from repo root):
    python/.venv/bin/python python/experiments/exp_002_doom/play.py --brain heuristic
    python/.venv/bin/python python/experiments/exp_002_doom/play.py --brain qwen3 --episodes 5
    python/.venv/bin/python python/experiments/exp_002_doom/play.py --brain heuristic \
        --episodes 200 --seed 1000 --record ~/data/doom/teacher_train.jsonl
"""

import argparse
import json
import random
import statistics as st
import time
from pathlib import Path

import yaml

from jeff.envs.doom import ACTION_NAMES, DoomEnv, DoomState, heuristic_action
from jeff.models.base import DecisionInput

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parent.parent.parent


class RandomBrain:
    def __init__(self, seed: int):
        self.rng = random.Random(seed)

    def __call__(self, state: DoomState, context: str):
        return self.rng.choice(ACTION_NAMES), None


class HeuristicBrain:
    def __init__(self, aim_margin: float):
        self.aim_margin = aim_margin

    def __call__(self, state: DoomState, context: str):
        return heuristic_action(state, self.aim_margin), None


class Qwen3Brain:
    def __init__(self, cfg: dict, device: str, model_name: str | None, adapter: str | None,
                 n_permutations: int | None):
        from jeff.models.qwen3_choice import Qwen3ChoiceConfig, Qwen3ChoiceModel

        m = cfg["model"]
        self.model = Qwen3ChoiceModel(Qwen3ChoiceConfig(
            model_name=model_name or m["name"],
            adapter=adapter or m.get("adapter"),
            device=device,
            load_in_4bit=m.get("load_in_4bit", True),
            n_permutations=m.get("n_permutations", 0) if n_permutations is None else n_permutations,
        ))

    def __call__(self, state: DoomState, context: str):
        out = self.model.decide(DecisionInput(context=context, options=state.option_texts()))
        return ACTION_NAMES[out.chosen_index], out.scores


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--brain", choices=["random", "heuristic", "qwen3"], required=True)
    ap.add_argument("--episodes", type=int, default=None)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--max-steps", type=int, default=None, help="cap decisions per episode (smoke tests)")
    ap.add_argument("--record", type=Path, default=None, help="write per-decision JSONL here")
    ap.add_argument("--tag", default=None, help="results file name (default: brain name)")
    ap.add_argument("--device", default="cuda")
    ap.add_argument("--model", default=None, help="override model.name (e.g. Qwen/Qwen3-0.6B)")
    ap.add_argument("--adapter", default=None, help="LoRA checkpoint dir")
    ap.add_argument("--n-permutations", type=int, default=None)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    env_cfg, ev = cfg["env"], cfg["eval"]
    episodes = args.episodes or ev["episodes"]
    seed = ev["seed"] if args.seed is None else args.seed
    aim_margin = cfg["teacher"]["aim_margin"]

    if args.brain == "random":
        brain = RandomBrain(seed)
    elif args.brain == "heuristic":
        brain = HeuristicBrain(aim_margin)
    else:
        brain = Qwen3Brain(cfg, args.device, args.model, args.adapter, args.n_permutations)

    env = DoomEnv(env_cfg["scenario"], env_cfg["tics_per_action"], seed=seed)
    env.game.set_seed(seed)   # measure_turn_rate() consumed an episode; restart the seed stream
    rec = None
    if args.record:
        args.record = args.record.expanduser()
        args.record.parent.mkdir(parents=True, exist_ok=True)
        rec = open(args.record, "w")

    kills, rewards, lengths, shots, agree, latency = [], [], [], 0, [], []
    for ep in range(episodes):
        state = env.reset()
        step = 0
        while not env.done and (args.max_steps is None or step < args.max_steps):
            context = state.describe(env_cfg["max_monsters"], env_cfg["include_physics"])
            teacher = heuristic_action(state, aim_margin)
            t0 = time.perf_counter()
            action, scores = brain(state, context)
            latency.append(time.perf_counter() - t0)
            agree.append(action == teacher)
            reward = env.step(action)
            nxt = env.observe()
            if not env.done:
                shots += max(0, state.ammo - nxt.ammo)   # bullets actually fired
            if rec:
                rec.write(json.dumps({
                    "context": context,
                    "options": state.option_texts(),
                    "correct_index": ACTION_NAMES.index(teacher),
                    "action": action,
                    "scores": scores,
                    "reward": reward,
                    "episode": ep, "step": step, "seed": seed, "brain": args.brain,
                }) + "\n")
            step += 1
            state = nxt
        kills.append(env.kills)
        rewards.append(env.total_reward)
        lengths.append(step)
        if not args.quiet:
            print(f"episode {ep + 1:3d}: kills {env.kills:2d}  reward {env.total_reward:+.0f}  "
                  f"decisions {step}")
    env.close()
    if rec:
        rec.close()

    result = {
        "brain": args.brain,
        "model": args.model or (cfg["model"]["name"] if args.brain == "qwen3" else None),
        "adapter": args.adapter,
        "episodes": episodes, "seed": seed, "tics_per_action": env_cfg["tics_per_action"],
        "max_steps": args.max_steps,
        "kills_mean": st.mean(kills), "kills_sd": st.pstdev(kills),
        "reward_mean": st.mean(rewards),
        "decisions_mean": st.mean(lengths),
        "kills_per_bullet": sum(kills) / shots if shots else 0.0,
        "teacher_agreement": sum(agree) / len(agree) if agree else 0.0,
        "ms_per_decision": 1000 * st.mean(latency) if latency else 0.0,
        "kills": kills,
    }
    out = REPO_ROOT / cfg["output"]["results_dir"] / f"{args.tag or args.brain}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: (round(v, 3) if isinstance(v, float) else v)
                      for k, v in result.items() if k != "kills"}, indent=2))
    print(f"-> {out}" + (f"\n-> {args.record}" if rec else ""))


if __name__ == "__main__":
    main()
