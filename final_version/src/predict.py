import json
from pathlib import Path

import joblib
import pandas as pd


BASE_DIR = Path(__file__).resolve().parent.parent

DATA_PATH = BASE_DIR / "data" / "processed" / "processed_telemetry.csv"
MODELS_DIR = BASE_DIR / "models"

CLASSIFIER_PATH = MODELS_DIR / "failure_classifier.joblib"
REGRESSOR_PATH = MODELS_DIR / "rul_regressor.joblib"


FEATURES = [
    "erase_count",
    "retention_ticks",
    "temp_c",
    "garbage_ratio",
    "valid_pages",
    "invalid_pages",
    "rber_rolling_mean",
    "error_bits_rolling_mean",
    "rber_slope",
    "error_bits_slope",
]


def main():
    if not DATA_PATH.exists():
        raise FileNotFoundError(
            f"Processed telemetry not found: {DATA_PATH}"
        )

    if not CLASSIFIER_PATH.exists():
        raise FileNotFoundError(
            f"Classifier not found: {CLASSIFIER_PATH}"
        )

    if not REGRESSOR_PATH.exists():
        raise FileNotFoundError(
            f"RUL regressor not found: {REGRESSOR_PATH}"
        )

    df = pd.read_csv(DATA_PATH)

    classifier = joblib.load(CLASSIFIER_PATH)
    regressor = joblib.load(REGRESSOR_PATH)

    latest = (
        df.sort_values("tick")
        .groupby("block_id", as_index=False)
        .tail(1)
        .copy()
    )

    X = latest[FEATURES]

    failure_probability = classifier.predict_proba(X)[:, 1]
    failure_prediction = classifier.predict(X)
    rul_prediction = regressor.predict(X)

    results = []

    for i, (_, row) in enumerate(latest.iterrows()):
        probability = float(failure_probability[i])
        failure = int(failure_prediction[i])
        rul = max(0.0, float(rul_prediction[i]))

        # Combined risk assessment:
        # classifier probability + predicted remaining useful life
        if probability >= 0.70 or rul <= 5:
            risk = "HIGH"
        elif probability >= 0.30 or rul <= 20:
            risk = "MEDIUM"
        else:
            risk = "LOW"

        # Decision-support recommendation
        if risk == "HIGH" and rul <= 5:
            recommendation = "RISK-AWARE GC"
        elif risk == "HIGH":
            recommendation = "PRIORITIZE MONITORING"
        elif risk == "MEDIUM" and rul <= 20:
            recommendation = "MONITOR CLOSELY"
        else:
            recommendation = "NORMAL OPERATION"

        results.append({
            "block_id": int(row["block_id"]),
            "erase_count": int(row["erase_count"]),
            "rber": float(row["rber"]),
            "temperature": int(row["temp_c"]),
            "error_bits": int(row["error_bits"]),
            "failure_probability": round(probability, 4),
            "failure_prediction": failure,
            "predicted_rul": round(rul, 1),
            "risk": risk,
            "recommendation": recommendation
        })

    output = {
        "blocks": results,
        "block_count": len(results)
    }

    print(json.dumps(output))


if __name__ == "__main__":
    main()