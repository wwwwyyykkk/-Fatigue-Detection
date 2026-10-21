# Host-side tests

These tests run on a PC and cover pure logic from `main/perclos.hpp`.
They do not need ESP-IDF, a board, or a camera.

`host_test/mocks/esp_log.h` provides no-op logging macros so the header can be
included without the ESP-IDF runtime.

## Run

```powershell
cmake -S host_test -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## Coverage

- Empty / partially filled window
- 15% and 30% fatigue boundaries
- Sliding-window replacement
- Weather-linked thresholds (clear / rain / fog)
- Reset behavior
