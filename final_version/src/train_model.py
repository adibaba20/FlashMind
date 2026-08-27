"""
Module 5: ML/DL Prediction Model
==================================
Trains two models on Module 4's processed_telemetry.csv:

  1. CLASSIFIER — predicts `uncorrectable` (is this block failing right now?)
  2. REGRESSOR  — predicts `cycles_until_failure` (how many P/E cycles of
                  life does this block have left? i.e. Remaining Useful Life)

IMPORTANT METHODOLOGY NOTE — read this before trusting any accuracy number:
Rows from the SAME block are highly correlated (a block's history is one
continuous story). If we split train/test by ROW, the model could see a
block's cycle 50 in training and cycle 51 in testing — that's leakage, and
it would make accuracy look better than it really is. So this script splits
by BLOCK: entire blocks go to either train or test, never both. With only
6 blocks total (Module 3's demo SSD), this split is small — that's an
honest limitation of this dataset's scale, not the methodology, and it's
exactly why Module 3's stress test should be scaled up (more blocks, more
writes) before quoting these numbers as a real result.

We also deliberately EXCLUDE raw `error_bits` from the feature set: the
label `uncorrectable` is a direct threshold on `error_bits`, so including
it would let the model "cheat" by nearly reading the answer off a feature
instead of actually predicting from wear/retention/temperature trends.

Run: python3 train_model.py
Produces: model_evaluation.png, feature_importance.png, trained models (.joblib)
"""

import pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import joblib

from sklearn.model_selection import GroupShuffleSplit
from sklearn.metrics import (
    accuracy_score, precision_score, recall_score, f1_score,
    confusion_matrix, mean_absolute_error, mean_squared_error, r2_score
)
from xgboost import XGBClassifier, XGBRegressor

from pathlib import Path
BASE_DIR = Path(__file__).resolve().parent.parent # same folder as this script
DATA_PATH = BASE_DIR / "data" / "processed" / "processed_telemetry.csv"
MODELS_DIR = BASE_DIR / "models"
OUTPUTS_DIR = BASE_DIR / "outputs"
MODELS_DIR.mkdir(parents=True, exist_ok=True)
OUTPUTS_DIR.mkdir(parents=True, exist_ok=True)

# Features available to BOTH models — notice error_bits and rber (the raw,
# current-snapshot readings) are excluded on purpose; see module docstring.
FEATURES = [
    "erase_count", "retention_ticks", "temp_c", "garbage_ratio",
    "valid_pages", "invalid_pages",
    "rber_rolling_mean", "error_bits_rolling_mean",
    "rber_slope", "error_bits_slope",
]


def block_level_split(df: pd.DataFrame, group_col: str, test_size: float, seed: int):
    """
    Splits by whole block, never mixing one block's rows across train/test.
    Returns boolean masks (train_mask, test_mask).
    """
    splitter = GroupShuffleSplit(n_splits=1, test_size=test_size, random_state=seed)
    train_idx, test_idx = next(splitter.split(df, groups=df[group_col]))
    train_mask = df.index.isin(df.index[train_idx])
    test_mask = df.index.isin(df.index[test_idx])
    return train_mask, test_mask


def train_classifier(df: pd.DataFrame):
    print("\n" + "=" * 60)
    print("MODEL A: failure classifier (predicts `uncorrectable`)")
    print("=" * 60)

    train_mask, test_mask = block_level_split(df, "block_id", test_size=0.34, seed=42)
    train_blocks = sorted(df.loc[train_mask, "block_id"].unique())
    test_blocks = sorted(df.loc[test_mask, "block_id"].unique())
    print(f"Train blocks: {train_blocks}   Test blocks: {test_blocks}")
    print("(small split — only 6 blocks exist in this demo dataset; "
          "see module docstring on scaling this up before trusting the number)")

    X_train, y_train = df.loc[train_mask, FEATURES], df.loc[train_mask, "uncorrectable"]
    X_test, y_test = df.loc[test_mask, FEATURES], df.loc[test_mask, "uncorrectable"]

    print(f"Train rows: {len(X_train)} (failure rate {y_train.mean()*100:.1f}%)")
    print(f"Test rows:  {len(X_test)} (failure rate {y_test.mean()*100:.1f}%)")

    model = XGBClassifier(
        n_estimators=150, max_depth=3, learning_rate=0.1,
        scale_pos_weight=(y_train == 0).sum() / max((y_train == 1).sum(), 1),  # class imbalance is real (~4% positive)
        eval_metric="logloss", random_state=42,
    )
    model.fit(X_train, y_train)

    preds = model.predict(X_test)
    probs = model.predict_proba(X_test)[:, 1]

    acc = accuracy_score(y_test, preds)
    prec = precision_score(y_test, preds, zero_division=0)
    rec = recall_score(y_test, preds, zero_division=0)
    f1 = f1_score(y_test, preds, zero_division=0)
    cm = confusion_matrix(y_test, preds)

    print(f"\nAccuracy:  {acc:.3f}")
    print(f"Precision: {prec:.3f}  (of predicted failures, how many were real)")
    print(f"Recall:    {rec:.3f}  (of real failures, how many we caught)")
    print(f"F1:        {f1:.3f}")
    print(f"Confusion matrix:\n{cm}")
    print("Note: with ~4% positive class, a model that always predicts "
          "'no failure' would still score ~96% accuracy — accuracy alone "
          "is misleading here. Precision/recall on the failure class is "
          "what actually matters for this problem.")

    joblib.dump(model, MODELS_DIR / "failure_classifier.joblib")
    return model, X_test, y_test, preds, probs, cm


def train_regressor(df: pd.DataFrame):
    print("\n" + "=" * 60)
    print("MODEL B: RUL regressor (predicts cycles_until_failure)")
    print("=" * 60)

    labeled = df[df["rul_censored"] == 0].copy()
    print(f"Using {len(labeled)} non-censored rows "
          f"(rows from blocks that had a failure in this simulation run)")

    train_mask, test_mask = block_level_split(labeled, "block_id", test_size=0.34, seed=42)
    X_train = labeled.loc[train_mask, FEATURES]
    y_train = labeled.loc[train_mask, "cycles_until_failure"]
    X_test = labeled.loc[test_mask, FEATURES]
    y_test = labeled.loc[test_mask, "cycles_until_failure"]

    print(f"Train rows: {len(X_train)}   Test rows: {len(X_test)}")

    model = XGBRegressor(
        n_estimators=150, max_depth=3, learning_rate=0.1, random_state=42
    )
    model.fit(X_train, y_train)

    preds = model.predict(X_test)
    mae = mean_absolute_error(y_test, preds)
    rmse = np.sqrt(mean_squared_error(y_test, preds))
    r2 = r2_score(y_test, preds)

    print(f"\nMAE:  {mae:.1f} cycles  (average prediction error)")
    print(f"RMSE: {rmse:.1f} cycles")
    print(f"R^2:  {r2:.3f}")

    joblib.dump(model, MODELS_DIR / "rul_regressor.joblib")
    return model, X_test, y_test, preds


def make_plots(clf_model, cm, reg_model, y_test_reg, preds_reg):
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))

    # 1. Confusion matrix
    im = axes[0].imshow(cm, cmap="Blues")
    axes[0].set_xticks([0, 1]); axes[0].set_xticklabels(["No failure", "Failure"])
    axes[0].set_yticks([0, 1]); axes[0].set_yticklabels(["No failure", "Failure"])
    axes[0].set_xlabel("Predicted"); axes[0].set_ylabel("Actual")
    axes[0].set_title("Classifier: confusion matrix")
    for i in range(2):
        for j in range(2):
            axes[0].text(j, i, str(cm[i, j]), ha="center", va="center",
                         color="white" if cm[i, j] > cm.max() / 2 else "black")

    # 2. RUL predicted vs actual
    axes[1].scatter(y_test_reg, preds_reg, alpha=0.5, s=15)
    lims = [0, max(y_test_reg.max(), preds_reg.max()) * 1.05]
    axes[1].plot(lims, lims, "r--", linewidth=1, label="perfect prediction")
    axes[1].set_xlabel("Actual RUL (cycles)")
    axes[1].set_ylabel("Predicted RUL (cycles)")
    axes[1].set_title("Regressor: predicted vs actual RUL")
    axes[1].legend()

    # 3. Feature importance (classifier)
    importances = clf_model.feature_importances_
    order = np.argsort(importances)
    axes[2].barh(np.array(FEATURES)[order], importances[order])
    axes[2].set_title("Classifier: feature importance")
    axes[2].set_xlabel("Importance")

    fig.tight_layout()
    fig.savefig(OUTPUTS_DIR / "model_evaluation.png", dpi=120)
    print("\nSaved -> model_evaluation.png")


def main():
    df = pd.read_csv(DATA_PATH)
    print(f"Loaded {len(df)} rows, {df['block_id'].nunique()} blocks")

    clf_model, X_test_c, y_test_c, preds_c, probs_c, cm = train_classifier(df)
    reg_model, X_test_r, y_test_r, preds_r = train_regressor(df)

    make_plots(clf_model, cm, reg_model, y_test_r.values, preds_r)

    print("\nSaved trained models -> failure_classifier.joblib, rul_regressor.joblib")
    print("These are what Module 6 loads to make the FTL's GC/wear-leveling "
          "decisions risk-aware.")


if __name__ == "__main__":
    main()
