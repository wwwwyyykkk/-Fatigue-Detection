# ESP32-S3 Fatigue Detection Implementation Notes

## Final direction

Use one image classification model only:

- `eye_open`
- `eye_closed`

Do not use yawning as the core fatigue signal on ESP32-S3. Mouth detection requires a second ROI and usually a second model, which increases RAM/flash pressure and makes live demonstration less stable.

## Live demonstration flow

1. Mount the camera at a fixed angle and distance.
2. Show a face alignment frame on the preview screen.
3. Ask the user to keep the face inside that frame.
4. Crop the eye ROI from the aligned face area.
5. Run the eye model on that ROI.
6. Feed the result into PERCLOS.

Recommended fatigue rules:

- `PERCLOS <= 15%`: normal
- `15% < PERCLOS <= 30%`: mild fatigue
- `PERCLOS > 30%`: severe fatigue / alarm

`PERCLOS = closed_eye_frames / valid_eye_frames` in the recent time window.

## Dataset preparation

Your current screenshots include many crops that do not reliably contain eyes. Those should not be used directly. Build the dataset from stable eye boxes instead.

This workspace contains an annotated public dataset with labels:

- `mata_terbuka`: open eye
- `mata_terpejam`: closed eye

Run:

```powershell
python tools\prepare_eye_dataset.py `
  --source-dir "Dataset\Annotated Drowsiness Detection Dataset Captured Us\modelling\training\training" `
  --labels "Dataset\Annotated Drowsiness Detection Dataset Captured Us\modelling\training\training\bounding_boxes.labels" `
  --output-dir "output\eye_dataset" `
  --size 96
```

Upload `output\eye_dataset\eye_open` and `output\eye_dataset\eye_closed` to Edge Impulse as a two-class image classification dataset.

If the generated classes are imbalanced, create a balanced copy:

```powershell
python tools\balance_eye_dataset.py `
  --input-dir "output\eye_dataset" `
  --output-dir "output\eye_dataset_balanced"
```

Upload `output\eye_dataset_balanced` first. Then add your own newly collected closed-eye images to improve robustness.

## Defense explanation

The system uses eye closure as the main fatigue feature. PERCLOS measures the percentage of time that eyes remain closed in a recent time window, so it is more stable than single-frame classification. Yawning detection is left as an optional extension because it requires a separate mouth ROI and increases embedded inference cost.
