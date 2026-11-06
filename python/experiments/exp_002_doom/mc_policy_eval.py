"""Experiment 002 step 3 diagnostic — play with the Monte-Carlo lookahead policy itself.

At every decision each action is scored as in label_q.py: the state is restored by
seeded replay of the action prefix (ViZDoom save/load perturbs the game, see
jeff.envs.doom.replay) (K rollouts of the eps-noise teacher to episode end, common random
numbers). The policy takes an action that beats the teacher's by the same
significance rule as build_prefs.py, else the teacher's action. This is the
upper bound on what DPO on those preference pairs can teach: if it does not beat
the teacher in-game, the labels carry no exploitable signal.

Runs one episode per worker on seeds seed..seed+episodes-1, and the clean teacher on
the same seeds for a paired comparison.

    python/.venv/bin/python python/experiments/exp_002_doom/mc_policy_eval.py --episodes 44
"""

import argparse
import json
import multiprocessing as mp
import os
import random
import statistics as st
import sys
import tempfile
from pathlib import Path

import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.envs.doom import ACTION_NAMES, DoomEnv, heuristic_action, replay, periodic_fire_action  # noqa: E402


def teacher_fn(state, margin, period):
    return periodic_fire_action(state, margin, period) if period else heuristic_action(state, margin)


def noisy(state, rng, eps, margin, period=None):
    if rng.random() < eps:
        return rng.choice(ACTION_NAMES)
    return teacher_fn(state, margin, period)


def play(job):
    seed, mode, a = job
    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))
    env_cfg, margin = cfg["env"], cfg["teacher"]["aim_margin"]
    period = cfg["teacher"]["fire_period"] if a["v2"] else None
    env = DoomEnv(env_cfg["scenario"], env_cfg["tics_per_action"], seed=seed, shot_timer=a["v2"])
    env.game.set_seed(seed)
    state, step, overrides, shots = env.reset(), 0, 0, 0
    taken: list[str] = []          # action prefix; branching = seeded replay of it
    while not env.done:
        teacher = teacher_fn(state, margin, period)
        action = teacher
        if mode == "mc":
            k0 = env.kills
            R = []
            for act in ACTION_NAMES:
                rets = []
                for j in range(a["k"]):
                    crng = random.Random(f"{seed}/{step}/{j}")
                    replay(env, seed, taken)
                    env.step(act)
                    while not env.done:
                        env.step(noisy(env.observe(), crng, a["cont_eps"], margin, period))
                    rets.append(env.kills - k0)
                R.append(rets)
            replay(env, seed, taken)
            t = ACTION_NAMES.index(teacher)
            best, best_gap = None, 0.0
            for i in range(3):
                if i == t:
                    continue
                d = [x - y for x, y in zip(R[i], R[t])]
                m, se = st.mean(d), max(st.stdev(d) / len(d) ** 0.5, a["se_floor"])
                if m >= a["min_gap"] and m > a["z"] * se and m > best_gap:
                    best, best_gap = i, m
            if best is not None:
                action, overrides = ACTION_NAMES[best], overrides + 1
        env.step(action)
        taken.append(action)
        nxt = env.observe()
        if not env.done:
            shots += max(0, state.ammo - nxt.ammo)
        state, step = nxt, step + 1
    kills = env.kills
    env.close()
    return {"seed": seed, "mode": mode, "kills": kills, "decisions": step,
            "overrides": overrides, "shots": shots}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--episodes", type=int, default=44)
    ap.add_argument("--seed", type=int, default=7000)
    ap.add_argument("--k", type=int, default=16)
    ap.add_argument("--cont-eps", type=float, default=0.1)
    ap.add_argument("--min-gap", type=float, default=0.5)
    ap.add_argument("--z", type=float, default=2.5)
    ap.add_argument("--se-floor", type=float, default=0.1, help="CRN can give se=0")
    ap.add_argument("--v2", action="store_true", help="obs v2 + periodic-fire teacher (and continuation)")
    ap.add_argument("--workers", type=int, default=max(1, os.cpu_count() - 2))
    ap.add_argument("--out", default=str(EXP_DIR / "results" / "mc_policy.json"))
    args = ap.parse_args()
    a = {"v2": args.v2, "k": args.k, "se_floor": args.se_floor, "cont_eps": args.cont_eps, "min_gap": args.min_gap, "z": args.z,
         "tmp_dir": tempfile.mkdtemp(prefix="doom_mc_")}
    jobs = [(args.seed + i, m, a) for i in range(args.episodes) for m in ("teacher", "mc")]
    with mp.Pool(args.workers) as pool:
        res = pool.map(play, jobs, chunksize=1)
    out = {}
    for m in ("teacher", "mc"):
        r = [x for x in res if x["mode"] == m]
        k = [x["kills"] for x in r]
        out[m] = {"kills_mean": st.mean(k), "kills_sd": st.pstdev(k),
                  "kills_per_bullet": sum(k) / max(1, sum(x["shots"] for x in r)),
                  "override_rate": sum(x["overrides"] for x in r) / sum(x["decisions"] for x in r)}
    diffs = [m["kills"] - t["kills"] for t, m in zip(
        sorted([x for x in res if x["mode"] == "teacher"], key=lambda x: x["seed"]),
        sorted([x for x in res if x["mode"] == "mc"], key=lambda x: x["seed"]))]
    out["paired_diff_mean"] = st.mean(diffs)
    out["paired_diff_se"] = st.stdev(diffs) / len(diffs) ** 0.5
    out["config"] = vars(args)
    out["episodes"] = res
    Path(args.out).write_text(json.dumps(out, indent=2) + "\n")
    print(json.dumps({k: v for k, v in out.items() if k not in ("episodes", "config")}, indent=2))


if __name__ == "__main__":
    main()
