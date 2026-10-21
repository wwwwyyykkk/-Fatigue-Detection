# Host-side unit tests

This folder contains PC-side tests for pure logic that does not depend on
Arduino, FreeRTOS, or hardware peripherals.

The first module, `perclos_level()`, mirrors the thresholds in
`drivesafe/config.h`:

| Ratio | Level |
|---|---|
| `<= 0.15` | normal |
| `<= 0.30` | mild |
| `> 0.30` | severe |
| outside `[0, 1]` | invalid (`-1`) |

## Run locally

```powershell
cmake -S host_test -B host_test/build
cmake --build host_test/build
ctest --test-dir host_test/build --output-on-failure
```

## Why this exists

The target firmware still talks to the camera, OLED, buzzer, and 4G module.
Those parts are verified on hardware. This folder tests the pure decision
logic on a PC so regressions are caught before flashing.
