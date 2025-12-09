# H2-Tank-Display

Display for MESCH H2 Tank Capacity

## PlatformIO

A ready-to-build PlatformIO environment is provided for the LilyGO T-Display S3 (Long). To compile and upload:

```bash
# Install PlatformIO if needed
pip install platformio

# Build
pio run

# Upload (board connected over USB)
pio run --target upload
```

The sketch source lives in `src/main.cpp` and uses TFT_eSPI build flags in `platformio.ini` that match the T-Display S3 Long pinout and 170x320 ST7789 panel.
