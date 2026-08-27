"""
Module 6: Feedback Loop -- ML-Augmented FTL
=============================================
This is the piece that makes the whole project more than "a systems
simulator" plus "an ML pipeline" sitting side by side. It closes the
loop: the failure classifier trained in Module 5 now INFLUENCES the
FTL's garbage-collection decisions in a live simulation.

Two policies are run on the SAME workload (same LBA write sequence,
same random seed) so the comparison is fair:

  BASELINE policy (matches Module 2's C simulator exactly):
    GC victim = the block with the most INVALID pages. Purely reactive
    -- it only cleans up garbage, it has no idea which blocks are
    becoming unreliable.

  ML-AUGMENTED policy:
    At every GC decision point, every block's current health features
    are fed into Module 5's trained classifier to get a live risk
    score. The victim is chosen using invalid-page count PLUS a risk
    bonus -- so a block that's accumulating real wear/error risk gets
    reclaimed (its data moved to a fresher block, then erased) sooner,
    even if it isn't the most garbage-heavy block yet. This trades a
    little extra wear (more erases) for fewer blocks lingering in a
    high-risk state.

Both policies share the exact same degradation model from Module 3
(RBER growing with erase count / retention / temperature), so the
comparison measures a real, if modest, effect -- not a rigged one.

Run: python3 feedback_loop.py   (needs models/*.joblib from Module 5)
Produces: outputs/feedback_loop_comparison.png
"""

import random
import math
from pathlib import Path
from collections import deque
from dataclasses import dataclass, field

import numpy as np
import joblib
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

BASE_DIR = Path(__file__).resolve().parent.parent
MODELS_DIR = BASE_DIR / "models"
OUTPUTS_DIR = BASE_DIR / "outputs"
OUTPUTS_DIR.mkdir(parents=True, exist_ok=True)

# ---------------------------------------------------------------------
# Same constants as the C simulator (Modules 1-3) -- kept identical so
# this Python re-simulation is a faithful stand-in, not a different toy.
# ---------------------------------------------------------------------
NUM_BLOCKS = 6
PAGES_PER_BLOCK = 6
TOTAL_PAGES = NUM_BLOCKS * PAGES_PER_BLOCK
BITS_PER_PAGE = 32 * 8
ECC_CORRECTABLE_BITS = 1
BASE_RBER = 0.00003
GC_FREE_PAGE_THRESHOLD = PAGES_PER_BLOCK

FEATURES = [
    "erase_count", "retention_ticks", "temp_c", "garbage_ratio",
    "valid_pages", "invalid_pages",
    "rber_rolling_mean", "error_bits_rolling_mean",
    "rber_slope", "error_bits_slope",
]

RISK_THRESHOLD = 0.5     # classifier probability above which a block is "high risk"
RISK_BONUS_WEIGHT = 8.0  # how strongly risk influences GC victim choice


def compute_rber(erase_count, retention_ticks, temp_c):
    wear_factor = (1.0 + erase_count / 40.0) ** 2.4
    retention_factor = (1.0 + retention_ticks / 300.0) ** 1.2
    temp_factor = 1.10 ** (temp_c - 40)
    return BASE_RBER * wear_factor * retention_factor * temp_factor


def simulate_bit_errors(rber, rng):
    return sum(1 for _ in range(BITS_PER_PAGE) if rng.random() < rber)


@dataclass
class Block:
    valid_lbas: set = field(default_factory=set)  # WHICH LBAs are live here (identity, not just a count)
    free_pages: int = PAGES_PER_BLOCK
    invalid_pages: int = 0
    erase_count: int = 0
    last_erase_tick: int = 0
    temp_c: int = 40
    error_hist: deque = field(default_factory=lambda: deque(maxlen=3))
    rber_hist: deque = field(default_factory=lambda: deque(maxlen=3))
    last_error_bits: float = 0.0
    last_rber: float = 0.0

    @property
    def valid_pages(self):
        return len(self.valid_lbas)


class FTLSim:
    """Block-level FTL simulation: tracks page counts per block, not
    individual page data (we don't need data content for this study,
    only wear/garbage state -- keeps this fast and focused)."""

    def __init__(self, policy, classifier=None, seed=0):
        self.policy = policy  # "baseline" or "ml"
        self.classifier = classifier
        self.blocks = [Block() for _ in range(NUM_BLOCKS)]
        self.lba_to_block = {}  # mirrors the real FTL's LBA -> PPA mapping table
        self.tick = 0
        self.total_writes = 0
        self.total_physical_writes = 0
        self.total_erases = 0
        self.total_gc_cycles = 0
        self.total_uncorrectable_events = 0
        self.rng = random.Random(seed)

    def _risk_score(self, b: Block) -> float:
        if self.classifier is None:
            return 0.0
        retention_ticks = self.tick - b.last_erase_tick
        garbage_ratio = b.invalid_pages / max(b.valid_pages + b.invalid_pages, 1e-9)
        rber_roll = sum(b.rber_hist) / len(b.rber_hist) if b.rber_hist else 0.0
        err_roll = sum(b.error_hist) / len(b.error_hist) if b.error_hist else 0.0
        rber_slope = (b.rber_hist[-1] - b.rber_hist[-2]) if len(b.rber_hist) >= 2 else 0.0
        err_slope = (b.error_hist[-1] - b.error_hist[-2]) if len(b.error_hist) >= 2 else 0.0

        feat = np.array([[b.erase_count, retention_ticks, b.temp_c, garbage_ratio,
                           b.valid_pages, b.invalid_pages, rber_roll, err_roll,
                           rber_slope, err_slope]])
        return float(self.classifier.predict_proba(feat)[0, 1])

    def _select_target_block(self, exclude=-1):
        best, best_ec = -1, None
        for i, b in enumerate(self.blocks):
            if i == exclude or b.free_pages <= 0:
                continue
            if best_ec is None or b.erase_count < best_ec:
                best, best_ec = i, b.erase_count
        return best

    def _select_gc_victim(self):
        if self.policy == "baseline":
            best, best_score = -1, 0
            for i, b in enumerate(self.blocks):
                if b.invalid_pages > best_score:
                    best, best_score = i, b.invalid_pages
            return best
        else:  # ml-augmented: invalid-page count PLUS risk bonus
            best, best_score = -1, 0.0
            for i, b in enumerate(self.blocks):
                if b.invalid_pages == 0 and b.valid_pages == 0:
                    continue
                risk = self._risk_score(b)
                score = b.invalid_pages + (RISK_BONUS_WEIGHT * risk if risk > RISK_THRESHOLD else 0.0)
                if score > best_score:
                    best, best_score = i, score
            return best if best_score > 0 else -1

    def _erase_block(self, i):
        b = self.blocks[i]
        b.valid_lbas.clear()
        b.free_pages = PAGES_PER_BLOCK
        b.invalid_pages = 0
        b.erase_count += 1
        self.total_erases += 1
        self.tick += 1
        b.last_erase_tick = self.tick
        b.temp_c = 38 + self.rng.randint(0, 5)

    def _gc_run(self, victim_idx):
        victim = self.blocks[victim_idx]
        for lba in list(victim.valid_lbas):
            dest = self._select_target_block(exclude=victim_idx)
            if dest == -1:
                return False
            d = self.blocks[dest]
            d.valid_lbas.add(lba)
            d.free_pages -= 1
            self.lba_to_block[lba] = dest  # mapping MUST follow the data, or a later
                                            # overwrite would invalidate the wrong block
            self.total_physical_writes += 1
        self.total_gc_cycles += 1
        self._erase_block(victim_idx)
        return True

    def _gc_if_needed(self):
        total_free = sum(b.free_pages for b in self.blocks)
        while total_free < GC_FREE_PAGE_THRESHOLD:
            victim = self._select_gc_victim()
            if victim == -1:
                return
            if not self._gc_run(victim):
                return
            total_free = sum(b.free_pages for b in self.blocks)

    def _snapshot_health(self, b: Block):
        """Same telemetry math as Module 3's degradation.c, kept in sync
        on every write so risk features and ground-truth failures are
        computed identically across both policies."""
        retention_ticks = self.tick - b.last_erase_tick
        rber = compute_rber(b.erase_count, retention_ticks, b.temp_c)
        error_bits = simulate_bit_errors(rber, self.rng)
        uncorrectable = error_bits > ECC_CORRECTABLE_BITS

        b.rber_hist.append(rber)
        b.error_hist.append(error_bits)
        b.last_rber, b.last_error_bits = rber, error_bits

        if uncorrectable:
            self.total_uncorrectable_events += 1

    def write(self, lba):
        self._gc_if_needed()
        target = self._select_target_block()
        if target == -1:
            return False
        b = self.blocks[target]

        # Deterministic invalidation, matching the real FTL: if this LBA
        # already lives somewhere, that OLD physical page becomes garbage
        # right now -- not a random guess, an exact consequence of this
        # specific overwrite. This is what keeps write-in vs garbage-out
        # balanced at steady state (a hand-wavy random model doesn't).
        old_block = self.lba_to_block.get(lba)
        if old_block is not None:
            ob = self.blocks[old_block]
            ob.valid_lbas.discard(lba)
            ob.invalid_pages += 1

        b.valid_lbas.add(lba)
        b.free_pages -= 1
        self.lba_to_block[lba] = target

        self.total_writes += 1
        self.total_physical_writes += 1
        self.tick += 1
        self._snapshot_health(b)
        return True

    def write_amplification(self):
        return self.total_physical_writes / max(self.total_writes, 1)


def run_workload(policy, classifier, seed, num_writes, num_lbas):
    sim = FTLSim(policy, classifier, seed=seed)
    workload_rng = random.Random(seed + 1000)  # separate stream for LBA choice
    for _ in range(num_writes):
        lba = workload_rng.randrange(num_lbas)
        sim.write(lba)
    return sim


def main():
    clf_path = MODELS_DIR / "failure_classifier.joblib"
    if not clf_path.exists():
        raise FileNotFoundError(
            f"{clf_path} not found -- run train_model.py (Module 5) first."
        )
    classifier = joblib.load(clf_path)

    NUM_WRITES = 6000
    NUM_LBAS = 15
    SEED = 7

    print(f"Running {NUM_WRITES} writes across {NUM_LBAS} LBAs, "
          f"{NUM_BLOCKS} blocks x {PAGES_PER_BLOCK} pages...\n")

    baseline = run_workload("baseline", None, SEED, NUM_WRITES, NUM_LBAS)
    ml = run_workload("ml", classifier, SEED, NUM_WRITES, NUM_LBAS)

    def summarize(sim, name):
        print(f"--- {name} ---")
        print(f"Total erases:          {sim.total_erases}")
        print(f"GC cycles:             {sim.total_gc_cycles}")
        print(f"Write amplification:   {sim.write_amplification():.2f}x")
        print(f"Uncorrectable events:  {sim.total_uncorrectable_events}")
        erase_counts = [b.erase_count for b in sim.blocks]
        print(f"Erase count spread:    min={min(erase_counts)} "
              f"max={max(erase_counts)} (blocks: {erase_counts})")
        print()
        return {
            "erases": sim.total_erases,
            "gc_cycles": sim.total_gc_cycles,
            "write_amp": sim.write_amplification(),
            "uncorrectable": sim.total_uncorrectable_events,
        }

    b_stats = summarize(baseline, "BASELINE (Module 2 policy)")
    m_stats = summarize(ml, "ML-AUGMENTED (Module 6 policy)")

    delta = b_stats["uncorrectable"] - m_stats["uncorrectable"]
    pct = (delta / max(b_stats["uncorrectable"], 1)) * 100
    print("=" * 60)
    print(f"RESULT: ML-augmented FTL saw {m_stats['uncorrectable']} uncorrectable "
          f"events vs baseline's {b_stats['uncorrectable']} "
          f"({'-' if delta >= 0 else '+'}{abs(pct):.1f}%), "
          f"at {m_stats['write_amp']:.2f}x vs {b_stats['write_amp']:.2f}x "
          f"write amplification.")
    print("=" * 60)

    # --- comparison chart ---
    fig, axes = plt.subplots(1, 3, figsize=(14, 4.5))

    labels = ["Baseline", "ML-augmented"]
    axes[0].bar(labels, [b_stats["uncorrectable"], m_stats["uncorrectable"]],
                color=["#2a78d6", "#eb6834"])
    axes[0].set_title("Uncorrectable error events")
    axes[0].set_ylabel("Count")

    axes[1].bar(labels, [b_stats["write_amp"], m_stats["write_amp"]],
                color=["#2a78d6", "#eb6834"])
    axes[1].set_title("Write amplification")
    axes[1].set_ylabel("x")

    axes[2].bar(labels, [b_stats["erases"], m_stats["erases"]],
                color=["#2a78d6", "#eb6834"])
    axes[2].set_title("Total block erases")
    axes[2].set_ylabel("Count")

    fig.tight_layout()
    out_path = OUTPUTS_DIR / "feedback_loop_comparison.png"
    fig.savefig(out_path, dpi=120)
    print(f"\nSaved -> {out_path}")


if __name__ == "__main__":
    main()
