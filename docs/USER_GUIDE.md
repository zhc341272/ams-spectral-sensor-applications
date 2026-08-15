# Desktop application user guide

[中文](USER_GUIDE.zh-CN.md) · [Back to README](../README.md)

## 1. Install

Use Python 3.10 or newer. On Windows, the standard Python installer must include Tcl/Tk.

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

`Candidates` is the identity evidence reported by firmware. `Data profile` is the channel interpretation currently in use. A manual profile changes interpretation only and must not be treated as proof of an exact package part.

## 3. Take a measurement

### Read ambient

**Read ambient** turns all board LEDs off, waits for the dark-settle interval, and captures one independent snapshot. It replaces the previous four-source curves on screen.

### Measure all four LEDs

**Measure all four LEDs** processes 405 nm, white, 850 nm, and 940 nm in order. For each source the firmware:

1. runs a short per-source automatic-gain probe when enabled;
2. turns all sources off and waits 50 ms;
3. acquires the dark frame;
4. turns on one source and waits 50 ms;
5. acquires the lit frame;
6. turns the source off and reports lit, dark, and status values.

The actual value matrix below the charts displays ambient raw counts and `lit − dark` net counts for every returned channel. A dash means that source has not completed; it is different from a measured zero.

### Continuous acquisition

Set a period in milliseconds and click **Start streaming**. The firmware enforces a minimum period of 1000 ms. A full four-source sequence can take longer than the requested period; the next sequence begins only after the current one has finished.

## 4. Read the plots

- **Dark-subtracted response:** spectral filter channels plotted at their nominal peak wavelengths. `CLEAR` and `FD_RAW` are omitted because they do not have one center wavelength.
- **Peak-normalized spectral shape:** each source is divided by its own peak, useful for comparing shapes rather than brightness.
- **Normalized source-channel heatmap:** includes every reported channel. Each row is normalized independently.
- **Usable net signal fraction:** `sum(net) / sum(lit)` across wavelength channels. This is a signal-usefulness indicator, not statistical SNR.
- **Stability over time:** summed net wavelength channels divided by gain, recorded after complete sequences.
- **Temperature history:** board NTC readings versus elapsed time.

Red saturation flags mean at least one sample in that source pair reported analog, digital, or ASTATUS saturation. Reduce illumination, integration time, or gain before using saturated data quantitatively.

## 5. Configure acquisition

- **Automatic gain:** keeps an independent remembered gain for every LED source.
- **Fixed gain:** used when automatic gain is disabled. AS7341 supports gain indices through 512×; AS7343/TCS3448 also expose 1024× and 2048×.
- **ATIME / ASTEP:** determine integration time. The application displays the actual microseconds reported by firmware.
- **Data profile:** use `AUTO` normally. Select a manual profile only when package marking or purchasing records establish the exact optical variant.

## 6. LED and temperature page

Each LED can be switched independently for wiring checks. **Cycle test** turns on each source for 500 ms and then turns everything off. Do not use manual LED-on mode as a quantitative acquisition substitute because it does not create a paired dark frame.

The NTC panel shows ADC counts, millivolts, calculated resistance, and temperature. Continuous monitoring is independent of spectral streaming.

## 7. Configuration and tests

The details panel exposes raw identity and signature registers. Quick tests cover board status, sensor configuration, register read/write restoration, reinitialization, reset, forced sample, and full diagnostics.

Register writes can place the sensor in an invalid state. Record the original value, make one change at a time, and use **Force reinitialization** or power-cycle the board after experiments.

## 8. Export data

- **Export current CSV:** sensor identity followed by ambient, lit, dark, and net values per channel for all four sources.
- **Export stability CSV:** one row per source per completed sequence, including gain, flags, temperature, sum, 1×-gain normalized sum, and peak.
- **Save log:** stores timestamped transmitted and received protocol payloads.

CSV files use UTF-8 with BOM so wavelength/channel labels open correctly in common spreadsheet applications.

## 9. Language switching

Choose `中文` or `English` from the upper-right selector. Widgets and plots are rebuilt in the selected language while the serial object, sensor identity, measurements, histories, settings, and communication log remain in memory.

