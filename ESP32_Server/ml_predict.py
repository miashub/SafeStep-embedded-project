import sys
import json
import joblib
import pandas as pd

MODEL_PATH = "ml/safestep_binary_fall_model.joblib"

FEATURES = [
    "pressure_raw",
    "pressure_delta",
    "pressure_rise_speed",
    "pressure_filtered",
    "pressure_baseline",
    "fall_detected",
    "temperature",
    "humidity",
    "pir",
    "ldr",
    "dark",
    "bed_exit",
    "led",
    "pwm",
    "buzzer_output",
    "alert",
    "escalation_pending",
    "escalated",
    "escalation_time_left_ms"
]

model = joblib.load(MODEL_PATH)

payload = json.loads(sys.argv[1])

row = {}
for f in FEATURES:
    row[f] = payload.get(f, 0)

df = pd.DataFrame([row])

pred = int(model.predict(df)[0])

if hasattr(model, "predict_proba"):
    prob = float(model.predict_proba(df)[0][1])
else:
    prob = float(pred)

print(json.dumps({
    "ml_fall_prediction": pred,
    "ml_fall_probability": prob
}))