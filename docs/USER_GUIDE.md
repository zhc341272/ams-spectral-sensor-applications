# Desktop application guide

[中文](USER_GUIDE.zh-CN.md) · [Back to README](../README.md)

## Install and start

Python 3.10 or newer is required. On Windows, install Python with Tcl/Tk.

```powershell
cd host-software
python -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
python ams_spectral_sensor_app.py
```

On Windows, `install_dependencies.bat` installs the dependencies and `run.bat` starts the application.

## Connect the board

1. Connect the board by USB. Power-cycle it after replacing a sensor.
2. Select the board COM port and keep the baud rate at `115200`.
3. Click **Connect**. The application queries the sensor, temperature, and LED status.
4. Check the detected model, I²C address, channel count, and firmware version in the device summary.

Typical results are `0x39 / 10 channels` for AS7341, `0x39 / 14 channels` for AS7343, and `0x59 / 14 channels` for TCS3448. Identity registers alone cannot distinguish AS7341 from AS7341L or AS7343 from AS7343L. Select an L profile only when the package marking or purchasing record confirms it.

## Run an experiment

### 1. Enter sample information

The experiment name, sample ID, operator, and notes are stored with experiment records and CSV exports. The sample ID is also used in the suggested file name.

### 2. Choose scan averaging

Select 1, 3, 5, or 10 scans, then click **Acquire and save record**. The application runs complete four-source measurements and saves the averaged result as one experiment record.

Automatic gain may choose different gain or integration settings between scans. Before averaging, every scan is scaled to the exposure conditions of the final scan. ADC counts from different ranges are therefore not averaged directly.

### 3. Inspect the result

**Live spectra** is the primary plot. Only filtered channels with nominal center wavelengths are plotted. `CLEAR` and `FD_RAW` remain available in the heatmap and value table because they do not have one center wavelength.

**Values & analysis** shows the value of every channel under every source and reports:

- peak value and wavelength within the selected range;
- trapezoidal integral;
- positive-response-weighted centroid wavelength;
- lit/dark status flags.

The default analysis range is 400–900 nm and can be edited.

### 4. Export

**Export current CSV** writes experiment metadata, device identity, lit, dark, net, processed values, gain, integration time, temperature, and flags. **Save plot PNG** exports the primary graph.

CSV files use UTF-8 with BOM for direct use in Excel.

## Reference, relative response, and absorbance

The firmware already captures a paired dark frame for every source. For a relative measurement:

1. insert the reference or blank sample;
2. complete one acquisition;
3. click **Set current as reference**;
4. insert the test sample and acquire again;
5. select **Relative response (%)** or **Absorbance**.

Gain and integration time are normalized before the calculation:

```text
relative response = 100 × sample net response / reference net response
absorbance        = -log10(sample net response / reference net response)
net response      = lit - dark
```

The reference is cleared when the sensor profile, channel map, gain, auto-gain, or integration setting changes. This prevents processing measurements acquired under inconsistent conditions. A reference channel of zero produces a dash instead of an invalid value.

## Live acquisition

Set an interval and click **Start streaming** to continuously update the primary plot and stability series. Live mode does not create experiment records automatically. Use **Save current measurement** on the Records page to retain a useful snapshot.

The firmware starts a new sequence only after the previous one has completed, so a short interval does not interrupt a measurement.

## Records and comparison

Each **Acquire and save record** result appears in the record table. Select multiple records and choose the 405, white, 850, or 940 nm source to overlay them. When nothing is selected, the latest eight records are shown.

**Export records CSV** uses a long-table layout: each row represents one record, one source, and one sensor channel. It is suitable for Python, R, Origin, and statistical tools. Selected records are exported; when nothing is selected, all records are exported.

## Stability

Every complete measurement is appended to the stability series. The vertical axis is the summed net wavelength-channel signal normalized by actual gain and integration time to counts per second. Use it to check source warm-up, drift, and repeatability. The series can be exported separately.

## Device control and diagnostics

The Device page provides individual LED control, the cycle test, and NTC temperature monitoring. The Diagnostics page contains identity details, acquisition settings, register access, and firmware test commands.

Register writes are intended for debugging. Record the original value and run **Force reinitialization** or power-cycle the board afterward.

## Language

Choose `中文` or `English` at the upper right. The serial connection, current measurement, reference, experiment records, settings, and log are preserved.
