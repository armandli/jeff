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

    def describe(self, max_monsters: int = 6, include_physics: bool = True) -> str:
        """Render the state as the text context the model decides on."""
        lines = [f"Health: {self.health}", f"Ammo: {self.ammo}"]
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
                 seed: int | None = None, visible: bool = False):
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
        return self.observe()

    @property
    def done(self) -> bool:
        return self.game.is_episode_finished()

    def observe(self) -> DoomState:
        s = self.game.get_state()
        if s is None:   # episode finished
            return DoomState(health=0, ammo=0, turn_deg=self.turn_deg)
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
                         turn_deg=self.turn_deg)

    def step(self, action: str) -> float:
        vec = [int(b == action) for b in self.buttons]
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
