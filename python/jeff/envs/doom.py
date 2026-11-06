"""ViZDoom wrapper that exposes Doom as a sequence of text-only decisions.

The model never sees pixels. Each step the game state is reduced to a short
text description (health, ammo, and every live monster's distance and bearing
relative to where the player faces) and the model picks one button by name.

Scenario facts verified against ViZDoom 1.3.2 `defend_the_center`:
  - buttons (cfg order): TURN_LEFT, TURN_RIGHT, ATTACK; the player cannot move
  - 26 pistol bullets, 2100-tic timeout (60 s), monsters are melee-only Demons
  - reward is +1 per kill and -1 on death (matches KILLCOUNT exactly)
  - killed Demons are removed from the object list immediately (no corpses)
  - `MarineChainsawVzd` objects are static decoration even though their
    category is "Monster", so monsters are whitelisted by name
"""

from __future__ import annotations

import math
import os
from dataclasses import dataclass, field

MONSTER_NAMES = frozenset({
    "Demon", "Spectre", "ZombieMan", "ShotgunGuy", "ChaingunGuy", "DoomImp",
    "Cacodemon", "LostSoul", "HellKnight", "BaronOfHell", "Revenant",
})

# Actions offered to the model, in a fixed order. Descriptions are factual
# (what the button does), not a policy.
ACTIONS: dict[str, str] = {
    "ATTACK": "fire the pistol straight ahead (bearing 0)",
    "TURN_LEFT": "rotate left, toward monsters with positive bearing",
    "TURN_RIGHT": "rotate right, toward monsters with negative bearing",
}
ACTION_NAMES = list(ACTIONS)

DEMON_RADIUS = 30.0  # map units; Doom's Demon (pinky) actor radius


@dataclass
class Monster:
    kind: str
    distance: float      # map units
    bearing: float       # degrees, 0 = dead ahead, + = left, - = right

    @property
    def hit_half_angle(self) -> float:
        """Half the angle the monster's body subtends; |bearing| below this is on target."""
        return math.degrees(math.atan2(DEMON_RADIUS, max(self.distance, 1.0)))


@dataclass
class DoomState:
    health: int
    ammo: int
    monsters: list[Monster] = field(default_factory=list)   # nearest first
    turn_deg: float = 0.0     # degrees one turn decision rotates (from a standstill)
    since_shot: int | None = None   # decisions since the last ATTACK (None = not observed)

    def describe(self, max_monsters: int = 6, include_physics: bool = True) -> str:
        """Render the state as the text context the model decides on."""
        lines = [f"Health: {self.health}", f"Ammo: {self.ammo}"]
        if self.since_shot is not None:
            lines.append(f"Decisions since your last shot: {self.since_shot}")
        if self.monsters:
            lines.append("Monsters (nearest first):")
            for m in self.monsters[:max_monsters]:
                side = "ahead" if abs(m.bearing) < 1 else ("left" if m.bearing > 0 else "right")
                lines.append(f"- {m.kind}: distance {m.distance:.0f}, "
                             f"bearing {m.bearing:+.0f} deg ({side})")
        else:
            lines.append("Monsters: none")
        text = (
            "You are in Doom, standing in the center of a circular arena. You cannot "
            "move, only turn and shoot. Demons charge at you and attack in melee range.\n\n"
            + "\n".join(lines)
        )
        if include_physics:
            text += (
                "\n\nGame facts: bearing 0 is straight ahead, positive bearing is to your "
                "left, negative to your right. A Demon is about 60 units wide, so a shot "
                "hits only if its bearing is within about atan(30 / distance): roughly "
                "3 deg at distance 600, 6 deg at 300, 17 deg at 100. Missed shots waste "
                f"ammo you cannot get back. A turn action rotates you about {self.turn_deg:.0f} "
                f"deg, and about {2 * self.turn_deg:.0f} deg if you keep turning the same way."
            )
        return text

    def option_texts(self) -> list[str]:
        return [f"{name}: {desc}" for name, desc in ACTIONS.items()]


class DoomEnv:
    """Headless defend_the_center with text observations and named actions."""

    def __init__(self, scenario: str = "defend_the_center", tics_per_action: int = 2,
                 seed: int | None = None, visible: bool = False, shot_timer: bool = False):
        import vizdoom as vzd

        self.vzd = vzd
        GV = vzd.GameVariable
        g = vzd.DoomGame()
        g.load_config(os.path.join(vzd.scenarios_path, f"{scenario}.cfg"))
        g.set_window_visible(visible)
        g.set_mode(vzd.Mode.PLAYER)
        # Pixels are not used by the model; keep rendering cheap.
        g.set_screen_resolution(vzd.ScreenResolution.RES_160X120)
        g.set_screen_format(vzd.ScreenFormat.GRAY8)
        g.set_objects_info_enabled(True)
        g.set_available_game_variables([
            GV.HEALTH, GV.AMMO2, GV.POSITION_X, GV.POSITION_Y, GV.ANGLE, GV.KILLCOUNT,
        ])
        if seed is not None:
            g.set_seed(seed)
        g.init()
        self.game = g
        self.tics = tics_per_action
        self.shot_timer = shot_timer   # expose decisions-since-last-shot in the observation
        self._since_shot = 0
        self.buttons = [str(b).split(".")[-1] for b in g.get_available_buttons()]
        missing = set(ACTION_NAMES) - set(self.buttons)
        if missing:
            raise ValueError(f"scenario {scenario} lacks buttons {missing}; has {self.buttons}")
        # Doom turns slowly for the first few tics, then at full speed:
        # ~3.5 / 7 deg at 2 tics per decision, ~7 / 14 deg at 4 tics.
        self.turn_deg = 0.0
        self.turn_deg = abs(self.measure_turn_rate())

    # -- episode control ---------------------------------------------------
    def reset(self) -> DoomState:
        self.game.new_episode()
        self._since_shot = 0
        return self.observe()

    @property
    def done(self) -> bool:
        return self.game.is_episode_finished()

    def observe(self) -> DoomState:
        s = self.game.get_state()
        since = self._since_shot if self.shot_timer else None
        if s is None:   # episode finished
            return DoomState(health=0, ammo=0, turn_deg=self.turn_deg, since_shot=since)
        hp, ammo, px, py, ang, _kills = s.game_variables
        monsters = []
        for o in s.objects:
            if o.name not in MONSTER_NAMES:
                continue
            dx, dy = o.position_x - px, o.position_y - py
            bearing = (math.degrees(math.atan2(dy, dx)) - ang + 540.0) % 360.0 - 180.0
            monsters.append(Monster(o.name, math.hypot(dx, dy), bearing))
        monsters.sort(key=lambda m: m.distance)
        return DoomState(health=int(hp), ammo=int(ammo), monsters=monsters,
                         turn_deg=self.turn_deg, since_shot=since)

    def step(self, action: str) -> float:
        vec = [int(b == action) for b in self.buttons]
        self._since_shot = 0 if action == "ATTACK" else self._since_shot + 1
        return float(self.game.make_action(vec, self.tics))

    @property
    def kills(self) -> int:
        return int(self.game.get_game_variable(self.vzd.GameVariable.KILLCOUNT))

    @property
    def total_reward(self) -> float:
        return float(self.game.get_total_reward())

    def close(self) -> None:
        self.game.close()

    def measure_turn_rate(self) -> float:
        """Degrees rotated by one TURN_LEFT decision (for the physics note)."""
        self.reset()
        a0 = self.game.get_game_variable(self.vzd.GameVariable.ANGLE)
        self.step("TURN_LEFT")
        a1 = self.game.get_game_variable(self.vzd.GameVariable.ANGLE)
        return (a1 - a0 + 540.0) % 360.0 - 180.0


# ---------------------------------------------------------------------------
# Reference policies
# ---------------------------------------------------------------------------

def heuristic_action(state: DoomState, aim_margin: float = 1.0) -> str:
    """Aim-bot teacher: face the nearest Demon, fire only when the shot would hit.

    aim_margin scales the hit cone. Measured on 20 seeded episodes at 2 tics per
    decision: 1.0 -> 7.5 kills/episode; 1.3 and 1.7 are no better, and 0.5-0.7 are
    worse (waiting for a perfect shot lets Demons close in).
    """
    if not state.monsters:
        return "TURN_LEFT"          # scan for the next spawn
    m = state.monsters[0]
    if abs(m.bearing) <= m.hit_half_angle * aim_margin and state.ammo > 0:
        return "ATTACK"
    return "TURN_LEFT" if m.bearing > 0 else "TURN_RIGHT"


def periodic_fire_action(state: DoomState, aim_margin: float = 1.0, fire_period: int = 10) -> str:
    """Teacher v2: the aim-bot, plus a shot whenever none was fired for fire_period decisions.

    Needs state.since_shot (DoomEnv(shot_timer=True)). Measured on 200 episodes, seed 11:
    period 10 -> 9.26 kills/ep vs 7.09 for heuristic_action (period 5: 8.65, 20: 8.76,
    40: 7.84). Random extra shots help too (10% -> 9.06) and random turns hurt, so
    spare ammo is worth spending: the plain aim-bot dies with most of its 26 bullets.
    """
    if state.since_shot is not None and state.since_shot >= fire_period and state.ammo > 0:
        return "ATTACK"
    return heuristic_action(state, aim_margin)


def replay(env: DoomEnv, seed: int, actions: list[str]) -> DoomState:
    """Restore a game state by re-seeding and replaying an action prefix.

    Use this, not ViZDoom save()/load(), to branch from a state: in 1.3.2 on
    defend_the_center, save() alone lowers the aim-bot from 7.2 to 5.2 kills/ep, and
    save()+load() every decision drops it to 0.25, whereas seeded replay is exactly
    deterministic (identical health/ammo/kill traces). The shot timer is recomputed.
    """
    env.game.set_seed(seed)
    env.reset()
    for a in actions:
        env.step(a)
    return env.observe()
