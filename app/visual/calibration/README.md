# Cabin inspection calibration

These files are **field-test candidates**, not production acceptance records.

- `station_172_17_101_78_candidate.json` and its five PNG templates came from the stable no-plate recording at 160 s, 1080 x 1280. The JSON uses normalized left/top/right/bottom ROIs and relative template paths.
- `cabin_door_candidate.json` contains near-black NV12 Y thresholds. Limited-range NV12 black has Y near 16. A non-black frame does not prove that the door is closed.
- The historical partially blocked frame can still appear as no-code to the plate detector. SOC must request plate inspection only after door motion is complete and the work area is visible.

Use the production camera's NV12 path to verify resolution, orientation, exposure and the inspection position before SOC sends requests. Camera movement or a changed inspection position requires recalibration. Z control and readiness belong to SOC. This module does not add UI switches or alter machine-control logic.

The Visual process reads `UPC_CABIN_PLATE_CONFIG`, defaulting to `/userdata/visual/cabin_plate_config.json`. Copy a validated configuration **together with its template directory**, preserving their relative paths. Alternatively set the variable to the candidate JSON only for supervised testing. Missing or invalid calibration returns Unknown.

`UPC_CABIN_DOOR_CONFIG` points to the door JSON, defaulting to `/userdata/visual/cabin_door_config.json`. Both detectors require explicitly loaded, valid calibration. No built-in thresholds are enabled as a fallback: missing/invalid configuration or a failed reload returns Unknown. Every numeric field in these candidate JSON files is required; `min_medium_markers` must be an integer not exceeding the marker count. Marker ROIs must be inside the scene-quality search ROI and be at least 12 pixels wide/high at the reference resolution.

The plate detector matches calibrated laser-code image templates; it does not decode QR payloads. A single high-confidence marker or enough medium-confidence markers means code visible. Absence requires usable scene quality and all marker confidences below the hidden threshold; temporal confirmation is handled separately. Dust, reflections, tools, partial door obstruction, wrong positioning, or a covered code can still produce a false no-code result. The existing partial-door sample is a known failure, not a passed safety test. SOC must not treat this detector as door-closed feedback or request plate inspection during door motion.

Input buffers must be compact NV12 with even dimensions, at least 160 x 120, at most 8192 on either axis, and at most 16 megapixels total. Padded/strided buffers must be copied compactly by the caller. At most 32 templates and 16 megapixels of retained template data are allowed. Calibration files/assets are trusted local deployment inputs, not uploads from untrusted users. Frame sequence freshness, exposure warmup, cancellation, and request timeout are the service's responsibility, not the single-frame detector's.

`cabin_frame_check PLATE_CONFIG DOOR_CONFIG IMAGE...` converts offline images to compact NV12 and prints both results. This is a diagnostic harness, not real camera/IPC or hardware acceptance. Synthetic unit tests validate input handling and evidence logic only. Calibrate and accept with normal/abnormal production NV12 samples before enabling machine-side actions.

No build step copies these candidates into the device's active calibration or enables detection. Deployment and actual sensor-fault/door-motion tests remain field operations.
