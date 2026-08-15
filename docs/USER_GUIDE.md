# Desktop application guide

[中文](USER_GUIDE.zh-CN.md) · [Back to README](../README.md)

## 1. Install

Python 3.10 or newer is required. The Windows installation must include Tcl/Tk.

```powershell
cd host-software
python -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
python ams_spectral_sensor_app.py
```

You may instead double-click `install_dependencies.bat` and then `run.bat`.

## 2. Connect and identify the sensor

1. Connect the board USB cable and power-cycle the board after changing a sensor.
2. Start the application. Chinese is selected by default; choose **English** at the upper right if needed.
3. Select the board COM port and keep `115200` baud.
4. Click **Connect**. The application sends `PING`, `DETECT`, `TEMP`, and `LED STATUS` automatically.
5. Check the device summary:
   - AS7341 normally reports address `0x39`, 10 channels.
   - AS7343 normally reports address `0x39`, 14 channels.
   - TCS3448 normally reports address `0x59`, 14 channels.

`Candidates` comes from the identity registers. `Data profile` selects the channel map; changing it does not change the detection result.

## 3. Take a measurement

### Read ambient

**Read ambient** turns all LEDs off, waits 50 ms, and captures a snapshot. It replaces the current four-source curves.

### Measure all four LEDs

**Measure all four LEDs** processes 405 nm, white, 850 nm, and 940 nm in order. For each source the firmware:

1. runs a short per-source automatic-gain probe when enabled;
2. turns all sources off and waits 50 ms;
3. acquires the dark frame;
4. turns on one source and waits 50 ms;
5. acquires the lit frame;
6. turns the source off and reports lit, dark, and status values.

The value table shows ambient raw counts and `lit − dark` LED counts for every channel. A dash means no sample has been received.

### Continuous acquisition

Set a period and click **Start streaming**. The minimum is 1000 ms; a new sequence starts after the current one finishes.

## 4. Read the plots

- **Dark-subtracted response:** spectral filter channels plotted at their nominal peak wavelengths. `CLEAR` and `FD_RAW` are omitted because they do not have one center wavelength.
- **Peak-normalized spectral shape:** each source is divided by its own peak for shape comparison.
- **Normalized source-channel heatmap:** includes every reported channel. Each row is normalized independently.
- **Usable net signal fraction:** `sum(net) / sum(lit)` across wavelength channels; this is not statistical SNR.
- **Stability over time:** summed net wavelength channels divided by gain, recorded after complete sequences.
- **Temperature history:** board NTC readings versus elapsed time.

Red indicates saturation in the lit or dark sample. Reduce illumination, integration time, or gain and measure again.

## 5. Configure acquisition

- **Automatic gain:** keeps an independent remembered gain for every LED source.
- **Fixed gain:** used when automatic gain is disabled. AS7341 supports gain indices through 512×; AS7343/TCS3448 also expose 1024× and 2048×.
- **ATIME / ASTEP:** determine integration time. The application displays the actual microseconds reported by firmware.
- **Data profile:** use `AUTO` by default; select an `L` profile from package markings or purchasing records.

## 6. LED and temperature page

Each LED can be switched independently. **Cycle test** turns on each source for 500 ms and then turns all sources off. Manual LED control does not capture a paired dark frame.

The NTC panel shows ADC counts, millivolts, calculated resistance, and temperature. Continuous monitoring is independent of spectral streaming.

## 7. Configuration and tests

The details panel exposes raw identity and signature registers. Quick tests cover board status, sensor configuration, register read/write restoration, reinitialization, reset, forced sample, and full diagnostics.

Record the original value before a register write. Use **Force reinitialization** or power-cycle the board after testing.

## 8. Export data

- **Export current CSV:** sensor identity followed by ambient, lit, dark, and net values per channel for all four sources.
- **Export stability CSV:** one row per source per completed sequence, including gain, flags, temperature, sum, 1×-gain normalized sum, and peak.
- **Save log:** stores timestamped transmitted and received protocol payloads.

CSV files use UTF-8 with BOM.

## 9. Language switching

Choose `中文` or `English` at the upper right. The serial connection, measurements, settings, and log are retained.
