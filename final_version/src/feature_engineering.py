"""
Module 4: ML Data Pipeline
===========================
Takes Module 3's raw telemetry.csv (one row = one block's health snapshot
at one point in simulated time) and turns it into a training-ready
dataset with engineered features and two label types:

  1. CLASSIFICATION label: `uncorrectable` (already in the raw data) —
     "is this snapshot already a failure event?"

  2. REGRESSION label: `cycles_until_failure` (we compute this) —
     "how many more P/E cycles until THIS block's first failure?"
     This is the more useful, more interesting label: predicting
     Remaining Useful Life (RUL) lets the FTL take action *before*
     a block fails, not just detect failure after the fact.

Run: python3 feature_engineering.py   (run from inside src/)
Produces: ../data/processed/processed_telemetry.csv (used by Module 5's model training)
          ../outputs/eda_plots.png (sanity-check visualizations)
"""

import pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")  # no display needed, just save to file
import matplotlib.pyplot as plt
from pathlib import Path

# ---------------------------------------------------------------------
# Folder layout (relative to this script's location in src/):
#   ../data/raw/telemetry.csv          <- input, Module 3's output
#   ../data/processed/                 <- this script's main output
#   ../outputs/                        <- plots / charts
# Using Path(__file__).parent means this works no matter where you run
# the script FROM, as long as the folder structure itself is intact.
# ---------------------------------------------------------------------
BASE_DIR = Path(__file__).resolve().parent.parent

RAW_PATH = BASE_DIR / "data" / "raw" / "telemetry.csv"
OUT_PATH = BASE_DIR / "data" / "processed" / "processed_telemetry.csv"
PLOT_PATH = BASE_DIR / "outputs" / "eda_plots.png"

# Make sure the output folders exist (won't error if they already do)
OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
PLOT_PATH.parent.mkdir(parents=True, exist_ok=True)


def load_raw(path: str) -> pd.DataFrame:
    df = pd.read_csv(path)
    df = df.sort_values(["block_id", "tick"]).reset_index(drop=True)
    print(f"Loaded {len(df)} raw snapshots across {df['block_id'].nunique()} blocks")
    return df


def engineer_features(df: pd.DataFrame) -> pd.DataFrame:
    """
    Adds trend-based features on top of the raw per-snapshot values.
    A single snapshot's error_bits count is noisy (Module 3 samples it
    stochastically) — but the TREND across recent snapshots is a much
    more reliable predictor than any single reading, exactly like real
    SMART-attribute-based failure prediction works.
    """
    df = df.copy()

    # Rolling stats computed PER BLOCK (each block's own history only —
    # never let one block's trend leak into another block's features)
    grouped = df.groupby("block_id")

    df["error_bits_rolling_mean"] = grouped["error_bits"].transform(
        lambda s: s.rolling(window=3, min_periods=1).mean()
    )
    df["rber_rolling_mean"] = grouped["rber"].transform(
        lambda s: s.rolling(window=3, min_periods=1).mean()
    )
    # slope: how fast is error_bits changing snapshot-to-snapshot?
    # a rising slope is a much stronger warning sign than a high but
    # STABLE error count.
    df["error_bits_slope"] = grouped["error_bits"].transform(
        lambda s: s.diff().fillna(0)
    )
    df["rber_slope"] = grouped["rber"].transform(
        lambda s: s.diff().fillna(0)
    )

    # garbage ratio: how much of this block is reclaimable garbage right
    # now — a proxy for how "hot" (frequently rewritten) this block is
    df["garbage_ratio"] = df["invalid_pages"] / (
        df["valid_pages"] + df["invalid_pages"] + 1e-9
    )

    return df


def add_rul_label(df: pd.DataFrame) -> pd.DataFrame:
    """
    For every block, find its FIRST uncorrectable-error snapshot (its
    first observed "failure"). For every snapshot of that block BEFORE
    that point, label it with cycles_until_failure = (erase_count at
    failure) - (erase_count at this snapshot).

    Blocks that never failed in the simulation are marked as censored
    (rul_censored=1) — this is standard survival-analysis terminology:
    we know they survived AT LEAST this long, but not exactly how much
    longer they'd have lasted. Module 5 will handle these appropriately
    (e.g. exclude from regression training, or use survival methods)
    rather than silently guessing a wrong number for them.
    """
    df = df.copy()
    df["cycles_until_failure"] = np.nan
    df["rul_censored"] = 1

    for block_id, group in df.groupby("block_id"):
        failure_rows = group[group["uncorrectable"] == 1]
        if failure_rows.empty:
            continue  # this block never failed -> stays censored

        first_failure_erase_count = failure_rows.iloc[0]["erase_count"]
        idx = group.index
        # only label snapshots at or before the first failure
        pre_failure_mask = group["erase_count"] <= first_failure_erase_count
        pre_idx = idx[pre_failure_mask]

        df.loc[pre_idx, "cycles_until_failure"] = (
            first_failure_erase_count - df.loc[pre_idx, "erase_count"]
        )
        df.loc[pre_idx, "rul_censored"] = 0

    labeled = df["rul_censored"].eq(0).sum()
    censored = df["rul_censored"].eq(1).sum()
    print(f"RUL labels: {labeled} usable rows, {censored} censored "
          f"(block never failed in this simulation run)")
    return df


def make_eda_plots(df: pd.DataFrame, path: str) -> None:
    fig, axes = plt.subplots(2, 2, figsize=(12, 9))

    # 1. RBER vs erase_count — the core physical relationship
    axes[0, 0].scatter(df["erase_count"], df["rber"], s=8, alpha=0.4)
    axes[0, 0].set_xlabel("Erase count (P/E cycles)")
    axes[0, 0].set_ylabel("RBER")
    axes[0, 0].set_title("Raw Bit Error Rate vs Wear")

    # 2. failure rate by wear bucket — the ML-relevant summary
    df["wear_bucket"] = (df["erase_count"] // 25) * 25
    fail_rate = df.groupby("wear_bucket")["uncorrectable"].mean() * 100
    axes[0, 1].bar(fail_rate.index.astype(str), fail_rate.values, width=0.8)
    axes[0, 1].set_xlabel("Erase count bucket")
    axes[0, 1].set_ylabel("Uncorrectable rate (%)")
    axes[0, 1].set_title("Failure Rate by Wear Level")
    axes[0, 1].tick_params(axis="x", rotation=45)

    # 3. RUL distribution for labeled (non-censored) rows
    labeled = df[df["rul_censored"] == 0]
    axes[1, 0].hist(labeled["cycles_until_failure"], bins=30)
    axes[1, 0].set_xlabel("Cycles until failure (RUL)")
    axes[1, 0].set_ylabel("Count")
    axes[1, 0].set_title("RUL Label Distribution (usable rows only)")

    # 4. error_bits trend for one representative block, as example
    example_block = df["block_id"].iloc[0]
    ex = df[df["block_id"] == example_block]
    axes[1, 1].plot(ex["erase_count"], ex["error_bits"], "o-", markersize=3, alpha=0.6, label="raw")
    axes[1, 1].plot(ex["erase_count"], ex["error_bits_rolling_mean"], "-", linewidth=2, label="rolling mean")
    axes[1, 1].set_xlabel("Erase count")
    axes[1, 1].set_ylabel("Error bits")
    axes[1, 1].set_title(f"Block {example_block}: Raw vs Smoothed Error Trend")
    axes[1, 1].legend()

    fig.tight_layout()
    fig.savefig(path, dpi=120)
    print(f"Saved EDA plots -> {path}")


def main():
    df = load_raw(RAW_PATH)
    df = engineer_features(df)
    df = add_rul_label(df)

    df.to_csv(OUT_PATH, index=False)
    print(f"Saved processed dataset -> {OUT_PATH} ({len(df)} rows, {len(df.columns)} columns)")

    make_eda_plots(df, PLOT_PATH)

    print("\nColumn summary:")
    print(df.dtypes)

    print("\nClass balance (uncorrectable):")
    print(df["uncorrectable"].value_counts(normalize=True).rename("fraction"))


if __name__ == "__main__":
    main()
