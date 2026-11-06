"""Experiment 002 step 3 — label states with Monte-Carlo action values from game outcomes.

For states visited by a behaviour policy (teacher with epsilon noise), the state is
restored by seeded replay of the action prefix, and each of the three actions is
tried, followed by a continuation policy until the episode ends:

    Q(s, a) = mean over K rollouts of (kills after s) when taking a, then following pi_c

pi_c is the teacher with epsilon-noise (stochastic, so K rollouts differ; also a
stronger policy than the clean teacher: 8.0 vs 7.45 kills/ep at eps 0.1). Dying
ends the episode, so it is priced in as lost future kills.

The output JSONL keeps the DecisionExample fields (correct_index = argmax Q) plus
q_mean / q_se / raw q_returns per option (rollout j shares its continuation noise
across actions — common random numbers — so paired differences have low variance),
from which build_prefs.py makes (chosen, rejected) pairs.

Labels in ~/data/doom/q_train.jsonl made before 2026-11-05 15:00 used ViZDoom
save()/load() instead, which perturbs the game (see jeff.envs.doom.replay) and are
INVALID. Regenerate them with this version.

    python/.venv/bin/python python/experiments/exp_002_doom/label_q.py \
        --episodes 240 --seed 3000 --out ~/data/doom/q_train.jsonl
"""

import argparse
import json
import multiprocessing as mp
import os
import random
import statistics as st
import sys
import tempfile
import time
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.envs.doom import ACTION_NAMES, DoomEnv, heuristic_action, periodic_fire_action, replay  # noqa: E402


def teacher_fn(state, margin, period):
    return periodic_fire_action(state, margin, period) if period else heuristic_action(state, margin)


def noisy(state, rng, eps, margin, period=None):
    if rng.random() < eps:
        return rng.choice(ACTION_NAMES)
    return teacher_fn(state, margin, period)


def run_episode(args_tuple):
    seed, a = args_tuple
    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    env_cfg, margin = cfg["env"], cfg["teacher"]["aim_margin"]
    period = cfg["teacher"]["fire_period"] if a["v2"] else None
    env = DoomEnv(env_cfg["scenario"], env_cfg["tics_per_action"], seed=seed, shot_timer=a["v2"])
    env.game.set_seed(seed)
    rng = random.Random(seed * 7 + 1)
    rows, taken = [], []
    state = env.reset()
    step = 0
    while not env.done:
        if step % a["stride"] == 0:
            k0 = env.kills
            q = []
            for act in ACTION_NAMES:
                rets = []
                for j in range(a["k"]):
                    # common random numbers: rollout j uses the same continuation
                    # noise for every first action, so Q differences are paired
                    crng = random.Random(f"{seed}/{step}/{j}")
                    replay(env, seed, taken)
                    env.step(act)
                    while not env.done:
                        env.step(noisy(env.observe(), crng, a["cont_eps"], margin, period))
                    rets.append(env.kills - k0)
                q.append(rets)
            replay(env, seed, taken)
            means = [st.mean(r) for r in q]
            ses = [st.stdev(r) / len(r) ** 0.5 if len(r) > 1 else 0.0 for r in q]
            rows.append({
                "context": state.describe(env_cfg["max_monsters"], env_cfg["include_physics"]),
                "options": state.option_texts(),
                "correct_index": max(range(3), key=means.__getitem__),
                "teacher_index": ACTION_NAMES.index(teacher_fn(state, margin, period)),
                "q_mean": means, "q_se": ses, "q_returns": q,
                "seed": seed, "step": step,
            })
        act = noisy(state, rng, a["behaviour_eps"], margin, period)
        env.step(act)
        taken.append(act)
        state = env.observe()
        step += 1
    env.close()
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--episodes", type=int, default=240)
    ap.add_argument("--seed", type=int, default=3000, help="episode i uses seed + i")
    ap.add_argument("--k", type=int, default=16, help="rollouts per action")
    ap.add_argument("--stride", type=int, default=2, help="label every n-th decision")
    ap.add_argument("--behaviour-eps", type=float, default=0.1)
    ap.add_argument("--cont-eps", type=float, default=0.1)
    ap.add_argument("--v2", action="store_true", help="obs v2 + periodic-fire teacher (behaviour, continuation, teacher_index)")
    ap.add_argument("--workers", type=int, default=max(1, os.cpu_count() - 2))
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()

    out = args.out.expanduser()
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp_dir = tempfile.mkdtemp(prefix="doom_q_")
    a = {"v2": args.v2, "k": args.k, "stride": args.stride, "behaviour_eps": args.behaviour_eps,
         "cont_eps": args.cont_eps, "tmp_dir": tmp_dir}
    jobs = [(args.seed + i, a) for i in range(args.episodes)]
    t0, n, done = time.time(), 0, 0
    with mp.Pool(args.workers) as pool, open(out, "w") as f:
        for rows in pool.imap_unordered(run_episode, jobs):
            for r in rows:
                f.write(json.dumps(r) + "\n")
            n += len(rows)
            done += 1
            if done % 10 == 0 or done == len(jobs):
                print(f"{done}/{len(jobs)} episodes, {n} states, {time.time() - t0:.0f}s", flush=True)
    print(f"-> {out}")


if __name__ == "__main__":
    main()
