#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""AMS 光谱传感器应用上位机。

适配固件：2.3.0-ams-spectral-application / 协议 2.1
上位机版本：3.0.0（中英文界面版）
支持的传感器协议族：
- AS7341 / AS7341L（0x39，手动 SMUX）
- AS7343 / AS7343L（0x39，自动 SMUX）
- TCS3448（0x59，AS7343 兼容寄存器协议）

依赖：pyserial、matplotlib。Tkinter 随标准 Python 安装。
"""

from __future__ import annotations

import csv
import queue
import re
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any, Callable, Optional

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

try:
    import serial
    import serial.tools.list_ports
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "缺少 pyserial / pyserial is missing: python -m pip install pyserial") from exc

try:
    from matplotlib import font_manager, rcParams
    from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
    from matplotlib.figure import Figure
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "缺少 matplotlib / matplotlib is missing: python -m pip install matplotlib") from exc


def _configure_matplotlib_chinese_font() -> str:
    """选择系统中可用的中文字体，避免 Matplotlib 标题和坐标轴显示方框。"""
    candidates = (
        "Microsoft YaHei", "SimHei", "Noto Sans CJK SC",
        "Source Han Sans SC", "WenQuanYi Micro Hei",
        "Arial Unicode MS", "AR PL UMing CN",
    )
    available = {item.name for item in font_manager.fontManager.ttflist}
    selected = next((name for name in candidates if name in available), "DejaVu Sans")
    rcParams["font.sans-serif"] = [selected, "DejaVu Sans"]
    rcParams["axes.unicode_minus"] = False
    return selected


MATPLOTLIB_FONT = _configure_matplotlib_chinese_font()
APP_VERSION = "3.0.0"
APP_NAME_ZH = "AMS 光谱传感器应用"
APP_NAME_EN = "AMS Spectral Sensor Applications"
DEFAULT_BAUD = 115200
DEFAULT_CHANNELS = [
    "F1_405", "F2_425", "FZ_450", "F3_475", "F4_515", "F5_550",
    "FY_555", "FXL_600", "F6_640", "F7_690", "F8_745", "NIR_855",
    "CLEAR", "FD_RAW",
]
LIGHTS = ("405", "WHITE", "850", "940")
GAIN_LABELS = [
    "0.5×", "1×", "2×", "4×", "8×", "16×", "32×", "64×",
    "128×", "256×", "512×", "1024×", "2048×",
]


@dataclass
class SpectrumFrame:
    sequence: int = 0
    source: str = ""
    gain_index: int = 0
    gain_x1000: int = 0
    atime: int = 0
    astep: int = 0
    tint_us: int = 0
    temperature_x10: int = 0
    light_flags: int = 0
    dark_flags: int = 0
    light: list[int] = field(default_factory=list)
    dark: list[int] = field(default_factory=list)

    @property
    def net(self) -> list[int]:
        return [max(0, a - b) for a, b in zip(self.light, self.dark)]


@dataclass
class SensorIdentity:
    status: str = "DISCONNECTED"
    family: str = "-"
    candidates: str = "-"
    protocol: str = "-"
    profile: str = "AUTO"
    effective_profile: str = "-"
    profile_ambiguous: str = "-"
    confidence: str = "-"
    address: str = "-"
    id_raw: str = "-"
    id_code: str = "-"
    revision: str = "-"
    auxiliary: str = "-"
    sig92: str = "-"
    sig5a: str = "-"
    sigcfg0: str = "-"
    sigd6: str = "-"
    channel_count: str = "-"
    firmware: str = "-"
    protocol_version: str = "-"

    def fingerprint(self) -> tuple[str, ...]:
        return (self.status, self.family, self.candidates, self.protocol,
                self.profile, self.effective_profile, self.address,
                self.id_raw, self.id_code)


class SerialLink:
    """按行收发串口数据，读线程只负责把完整报文送入界面队列。"""

    def __init__(self, event_queue: queue.Queue[tuple[str, Any]]) -> None:
        self.event_queue = event_queue
        self.port: Optional[serial.Serial] = None
        self.thread: Optional[threading.Thread] = None
        self.stop_event = threading.Event()
        self.write_lock = threading.Lock()

    @property
    def connected(self) -> bool:
        return self.port is not None and self.port.is_open

    def connect(self, port_name: str, baud: int) -> None:
        self.disconnect()
        self.port = serial.Serial(
            port=port_name,
            baudrate=baud,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=0.15,
            write_timeout=1.0,
        )
        self.stop_event.clear()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def disconnect(self) -> None:
        self.stop_event.set()
        port = self.port
        self.port = None
        if port is not None:
            try:
                port.close()
            except serial.SerialException:
                pass
        if self.thread is not None and self.thread.is_alive():
            self.thread.join(timeout=0.5)
        self.thread = None

    def send(self, command: str) -> None:
        if not self.connected:
            raise serial.SerialException("串口尚未连接 / serial port is not connected")
        command = command.strip()
        if not command:
            return
        data = (command + "\r\n").encode("ascii", errors="strict")
        with self.write_lock:
            assert self.port is not None
            self.port.write(data)
            self.port.flush()

    def _reader(self) -> None:
        assert self.port is not None
        port = self.port
        while not self.stop_event.is_set():
            try:
                raw = port.readline()
                if not raw:
                    continue
                text = raw.decode("utf-8", errors="replace").strip()
                if text:
                    self.event_queue.put(("line", text))
            except (serial.SerialException, OSError) as exc:
                if not self.stop_event.is_set():
                    self.event_queue.put(("serial_error", str(exc)))
                break


class AmsSpectralApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.geometry("1440x900")
        self.root.minsize(1180, 760)

        self.events: queue.Queue[tuple[str, Any]] = queue.Queue()
        self.serial = SerialLink(self.events)
        self.identity = SensorIdentity()
        self.last_identity_fingerprint: Optional[tuple[str, ...]] = None
        self.channels = list(DEFAULT_CHANNELS)
        self.ambient = [0] * len(self.channels)
        self.measurements: dict[str, SpectrumFrame] = {}
        self.temperature_history: deque[tuple[float, float]] = deque(maxlen=600)
        self.measurement_history: deque[dict[str, Any]] = deque(maxlen=200)
        self.current_measurement_sequence: Optional[int] = None
        self.stream_active = False
        self.crc_state: Optional[bool] = None
        self.led_states = {name: False for name in LIGHTS}
        self.sensor_monitor_job: Optional[str] = None
        self.temperature_monitor_job: Optional[str] = None

        self._make_variables()
        self._build_ui()
        self.refresh_ports()
        self._rebuild_spectrum_table()
        self._redraw_spectrum_plot()
        self._redraw_stability_plot()
        self._redraw_temperature_plot()

        self.root.after(50, self._process_events)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def _make_variables(self) -> None:
        self.language_code = "zh"
        self.language_var = tk.StringVar(value="中文")
        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        self.connection_var = tk.StringVar(value="未连接")
        self.raw_command_var = tk.StringVar()

        self.sensor_monitor_var = tk.BooleanVar(value=False)
        self.sensor_monitor_interval_var = tk.StringVar(value="5")
        self.temp_monitor_var = tk.BooleanVar(value=False)
        self.temp_interval_var = tk.StringVar(value="2")
        self.stream_interval_var = tk.StringVar(value="1000")
        self.profile_var = tk.StringVar(value="AUTO")
        self.profile_note_var = tk.StringVar(value="等待传感器识别")

        self.identity_vars: dict[str, tk.StringVar] = {
            name: tk.StringVar(value="-") for name in (
                "status", "family", "candidates", "protocol", "profile",
                "effective_profile", "profile_ambiguous", "confidence",
                "address", "id_raw", "id_code", "revision", "auxiliary",
                "sig92", "sig5a", "sigcfg0", "sigd6", "channel_count",
                "firmware", "protocol_version",
            )
        }
        self.identity_vars["status"].set("DISCONNECTED")

        self.temp_status_var = tk.StringVar(value="-")
        self.temp_status_code = "-"
        self.temp_raw_var = tk.StringVar(value="-")
        self.temp_mv_var = tk.StringVar(value="-")
        self.temp_resistance_var = tk.StringVar(value="-")
        self.temp_c_var = tk.StringVar(value="-")

        self.led_vars = {name: tk.StringVar(value="关闭") for name in LIGHTS}
        self.led_mask_var = tk.StringVar(value="00")

        self.autogain_var = tk.BooleanVar(value=True)
        self.gain_var = tk.StringVar(value=GAIN_LABELS[5])
        self.atime_var = tk.StringVar(value="29")
        self.astep_var = tk.StringVar(value="599")
        self.reg_bank_var = tk.StringVar(value="0")
        self.reg_addr_var = tk.StringVar(value="0x92")
        self.reg_value_var = tk.StringVar(value="0x00")

        self.last_frame_var = tk.StringVar(value="尚未采样")
        self.crc_status_var = tk.StringVar(value="CRC：-")

    def _tr(self, chinese: str, english: str) -> str:
        """返回当前界面语言对应的文字。协议字段本身不在这里翻译。"""
        return chinese if self.language_code == "zh" else english

    def _light_label(self, light: str) -> str:
        if light == "WHITE":
            return self._tr("白光", "White")
        return f"{light} nm"

    def _status_label(self, status: str) -> str:
        labels = {
            "DISCONNECTED": ("未连接", "Disconnected"),
            "OK": ("正常", "OK"),
            "FOUND": ("已识别", "Detected"),
            "NOT_FOUND": ("未找到", "Not found"),
            "UNKNOWN": ("未知", "Unknown"),
            "OPEN": ("开路", "Open circuit"),
            "SHORT": ("短路", "Short circuit"),
        }
        chinese, english = labels.get(status.upper(), (status, status))
        return self._tr(chinese, english)

    def _set_window_title(self) -> None:
        name = APP_NAME_ZH if self.language_code == "zh" else APP_NAME_EN
        author = self._tr("作者", "Author")
        self.root.title(f"{name} v{APP_VERSION}  {author}: Hongchen Zhang")

    def _change_language(self, _event: object = None) -> None:
        """重建界面文字和图表，串口对象与采样数据保持不变。"""
        target = "en" if self.language_var.get() == "English" else "zh"
        if target == self.language_code:
            return

        old_log = ""
        if hasattr(self, "log_text"):
            old_log = self.log_text.get("1.0", tk.END)
        self.language_code = target
        for child in self.root.winfo_children():
            child.destroy()
        self._build_ui()
        self.refresh_ports()
        self._rebuild_spectrum_table()
        self._update_identity_display()
        self._update_profile_options()
        self._update_gain_options()
        self._refresh_runtime_labels()
        self._redraw_spectrum_plot()
        self._redraw_stability_plot()
        self._redraw_temperature_plot()
        if old_log.strip():
            self.log_text.insert("1.0", old_log)

    def _refresh_runtime_labels(self) -> None:
        if self.serial.connected:
            port_name = self._selected_port_device()
            self.connection_var.set(
                self._tr(f"已连接：{port_name} @ {self.baud_var.get()}",
                         f"Connected: {port_name} @ {self.baud_var.get()}"))
        else:
            self.connection_var.set(self._tr("未连接", "Disconnected"))
        self.connect_button.configure(
            text=self._tr("断开", "Disconnect") if self.serial.connected
            else self._tr("连接", "Connect"))
        self.stream_button.configure(
            text=self._tr("停止连续读取", "Stop streaming") if self.stream_active
            else self._tr("开始连续读取", "Start streaming"))
        self.crc_status_var.set(
            self._tr("CRC：正常", "CRC: OK") if self.crc_state is True
            else self._tr("CRC：错误", "CRC: error") if self.crc_state is False
            else self._tr("CRC：-", "CRC: -"))
        for light in LIGHTS:
            self.led_vars[light].set(
                self._tr("开启", "On") if self.led_states[light]
                else self._tr("关闭", "Off"))
        if self.temp_status_code != "-":
            self.temp_status_var.set(self._status_label(self.temp_status_code))
        if self.measurements:
            sequence = max(frame.sequence for frame in self.measurements.values())
            self.last_frame_var.set(self._tr(
                f"当前测量序号 {sequence}，已收到 {len(self.measurements)}/4 路光源",
                f"Measurement {sequence}: {len(self.measurements)}/4 sources received"))
        elif any(self.ambient):
            self.last_frame_var.set(self._tr("环境光数据已读取", "Ambient data received"))
        else:
            self.last_frame_var.set(self._tr("尚未采样", "Not sampled yet"))

    def _build_ui(self) -> None:
        self._set_window_title()
        self._build_connection_bar()
        self.notebook = ttk.Notebook(self.root)
        self.notebook.pack(fill=tk.BOTH, expand=True, padx=6, pady=(0, 6))

        self.data_tab = ttk.Frame(self.notebook)
        self.io_tab = ttk.Frame(self.notebook)
        self.test_tab = ttk.Frame(self.notebook)
        self.log_tab = ttk.Frame(self.notebook)
        self.notebook.add(self.data_tab, text=self._tr("测量与图表", "Measurements & charts"))
        self.notebook.add(self.io_tab, text=self._tr("LED 与温度", "LED & temperature"))
        self.notebook.add(self.test_tab, text=self._tr("配置与测试", "Configuration & tests"))
        self.notebook.add(self.log_tab, text=self._tr("通信日志", "Communication log"))

        self._build_data_tab()
        self._build_io_tab()
        self._build_test_tab()
        self._build_log_tab()

    def _build_connection_bar(self) -> None:
        frame = ttk.LabelFrame(self.root, text=self._tr("串口连接", "Serial connection"))
        frame.pack(fill=tk.X, padx=6, pady=6)

        ttk.Label(frame, text=self._tr("串口：", "Port:")).pack(
            side=tk.LEFT, padx=(8, 2), pady=6)
        self.port_combo = ttk.Combobox(frame, textvariable=self.port_var,
                                       width=28, state="readonly")
        self.port_combo.pack(side=tk.LEFT, padx=2)
        ttk.Button(frame, text=self._tr("刷新", "Refresh"),
                   command=self.refresh_ports).pack(side=tk.LEFT, padx=4)

        ttk.Label(frame, text=self._tr("波特率：", "Baud rate:")).pack(
            side=tk.LEFT, padx=(14, 2))
        ttk.Combobox(frame, textvariable=self.baud_var, width=10,
                     values=("9600", "57600", "115200", "230400", "460800"),
                     state="readonly").pack(side=tk.LEFT, padx=2)

        self.connect_button = ttk.Button(
            frame, text=self._tr("连接", "Connect"), command=self.toggle_connection)
        self.connect_button.pack(side=tk.LEFT, padx=10)
        ttk.Label(frame, textvariable=self.connection_var).pack(side=tk.LEFT, padx=8)
        ttk.Label(frame, textvariable=self.crc_status_var).pack(side=tk.RIGHT, padx=10)

        self.language_combo = ttk.Combobox(
            frame, textvariable=self.language_var, values=("中文", "English"),
            width=9, state="readonly")
        self.language_combo.pack(side=tk.RIGHT, padx=(2, 8))
        self.language_combo.bind("<<ComboboxSelected>>", self._change_language)
        ttk.Label(frame, text=self._tr("界面语言：", "Language:")).pack(
            side=tk.RIGHT, padx=(8, 2))

    def _build_data_tab(self) -> None:
        center = ttk.Panedwindow(self.data_tab, orient=tk.HORIZONTAL)
        center.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

        sidebar = ttk.Frame(center, width=330)
        plot_frame = ttk.Frame(center)
        center.add(sidebar, weight=0)
        center.add(plot_frame, weight=1)
        self.root.after_idle(lambda: center.sashpos(0, 330))

        self._build_acquisition_panel(sidebar)
        self._build_identity_panel(sidebar)

        toolbar = ttk.Frame(plot_frame)
        toolbar.pack(fill=tk.X, pady=(0, 4))
        ttk.Label(toolbar, textvariable=self.last_frame_var).pack(
            side=tk.LEFT, fill=tk.X, expand=True)
        ttk.Button(toolbar, text=self._tr("导出当前数据 CSV", "Export current CSV"),
                   command=self.export_current_csv).pack(side=tk.RIGHT)
        ttk.Button(toolbar, text=self._tr("导出稳定性 CSV", "Export stability CSV"),
                   command=self.export_stability_csv).pack(side=tk.RIGHT, padx=4)

        self.analysis_notebook = ttk.Notebook(plot_frame)

        overview = ttk.Frame(self.analysis_notebook)
        stability = ttk.Frame(self.analysis_notebook)
        table_frame = ttk.Frame(self.analysis_notebook)
        self.analysis_notebook.add(
            overview, text=self._tr("当前批次光谱", "Current spectra"))
        self.analysis_notebook.add(
            stability, text=self._tr("时间稳定性", "Stability over time"))
        self.analysis_notebook.add(
            table_frame, text=self._tr("全部通道数据", "All channel data"))

        self.spectrum_figure = Figure(figsize=(11.2, 7.4), dpi=100)
        self.net_ax = self.spectrum_figure.add_subplot(221)
        self.normalized_ax = self.spectrum_figure.add_subplot(222)
        self.heatmap_ax = self.spectrum_figure.add_subplot(223)
        self.quality_ax = self.spectrum_figure.add_subplot(224)
        self.spectrum_canvas = FigureCanvasTkAgg(self.spectrum_figure, master=overview)
        self.spectrum_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        self.stability_figure = Figure(figsize=(11.2, 7.0), dpi=100)
        self.stability_ax = self.stability_figure.add_subplot(111)
        self.stability_canvas = FigureCanvasTkAgg(self.stability_figure, master=stability)
        self.stability_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        columns = ("channel", "ambient", "dark", "405", "white", "850", "940")
        self.spectrum_tree = ttk.Treeview(table_frame, columns=columns,
                                          show="headings")
        headings = {
            "channel": self._tr("通道", "Channel"),
            "ambient": self._tr("环境光", "Ambient"),
            "dark": self._tr("暗场", "Dark"),
            "405": self._tr("405净值", "405 net"),
            "white": self._tr("白光净值", "White net"),
            "850": self._tr("850净值", "850 net"),
            "940": self._tr("940净值", "940 net"),
        }
        for col in columns:
            self.spectrum_tree.heading(col, text=headings[col])
            self.spectrum_tree.column(col, width=120 if col != "channel" else 150,
                                      minwidth=80, anchor=tk.CENTER, stretch=True)
        y_scroll = ttk.Scrollbar(table_frame, orient=tk.VERTICAL,
                                 command=self.spectrum_tree.yview)
        self.spectrum_tree.configure(yscrollcommand=y_scroll.set)
        self.spectrum_tree.pack(side=tk.LEFT, fill=tk.BOTH, expand=True,
                                padx=(6, 0), pady=6)
        y_scroll.pack(side=tk.RIGHT, fill=tk.Y)

        matrix_frame = ttk.LabelFrame(
            plot_frame,
            text=self._tr(
                "图中实际数值（环境光为原始计数；各 LED 为亮场 − 暗场净值）",
                "Values plotted (ambient is raw; each LED is light minus dark)"))
        matrix_frame.pack(side=tk.BOTTOM, fill=tk.X, pady=(4, 0))
        self.value_matrix = ttk.Treeview(matrix_frame, show="headings", height=5)
        matrix_scroll = ttk.Scrollbar(matrix_frame, orient=tk.HORIZONTAL,
                                      command=self.value_matrix.xview)
        self.value_matrix.configure(xscrollcommand=matrix_scroll.set)
        self.value_matrix.pack(fill=tk.X, expand=True, padx=4, pady=(3, 0))
        matrix_scroll.pack(fill=tk.X, padx=4, pady=(0, 3))

        self.analysis_notebook.pack(side=tk.TOP, fill=tk.BOTH, expand=True)

    def _build_identity_panel(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text=self._tr("设备摘要", "Device summary"))
        frame.pack(fill=tk.BOTH, expand=True, pady=(0, 4))

        all_keys = (
            "status", "family", "candidates", "protocol", "profile",
            "effective_profile", "profile_ambiguous", "confidence", "address",
            "id_raw", "id_code", "revision_aux", "signatures", "channel_count",
            "firmware_protocol",
        )
        self.identity_display_vars = {
            key: tk.StringVar(value="-") for key in all_keys
        }
        labels = [
            (self._tr("状态", "Status"), "status"),
            (self._tr("识别型号", "Detected model"), "family"),
            (self._tr("候选型号", "Candidates"), "candidates"),
            (self._tr("I²C 地址", "I²C address"), "address"),
            (self._tr("芯片 ID", "Chip ID"), "id_raw"),
            (self._tr("数据配置", "Data profile"), "effective_profile"),
            (self._tr("通道数", "Channels"), "channel_count"),
            (self._tr("固件 / 协议", "Firmware / protocol"), "firmware_protocol"),
        ]
        for row, (title, key) in enumerate(labels):
            ttk.Label(frame, text=f"{title}：").grid(row=row, column=0, sticky=tk.E,
                                                   padx=(6, 4), pady=2)
            ttk.Label(frame, textvariable=self.identity_display_vars[key],
                      wraplength=215).grid(row=row, column=1, sticky=tk.W,
                                           padx=(0, 6), pady=2)
        frame.columnconfigure(1, weight=1)

        buttons = ttk.Frame(frame)
        buttons.grid(row=len(labels), column=0, columnspan=2, sticky=tk.EW,
                     padx=4, pady=(6, 4))
        ttk.Button(buttons, text=self._tr("自动识别", "Auto-detect"),
                   command=lambda: self.send_command("DETECT")).pack(
                       side=tk.LEFT, fill=tk.X, expand=True, padx=2)
        ttk.Button(buttons, text=self._tr("刷新信息", "Refresh info"),
                   command=lambda: self.send_command("INFO")).pack(
                       side=tk.LEFT, fill=tk.X, expand=True, padx=2)

        profile = ttk.Frame(frame)
        profile.grid(row=len(labels) + 1, column=0, columnspan=2, sticky=tk.EW,
                     padx=6, pady=(4, 2))
        ttk.Label(profile, text=self._tr("解析配置：", "Data profile:")).pack(side=tk.LEFT)
        self.profile_combo = ttk.Combobox(
            profile, textvariable=self.profile_var,
            values=("AUTO", "AS7341", "AS7341L", "AS7343", "AS7343L", "TCS3448"),
            state="readonly", width=11)
        self.profile_combo.pack(side=tk.LEFT, padx=4)
        ttk.Button(profile, text=self._tr("应用", "Apply"),
                   command=self.apply_profile).pack(side=tk.LEFT)
        ttk.Label(frame, textvariable=self.profile_note_var, wraplength=285).grid(
            row=len(labels) + 2, column=0, columnspan=2, sticky=tk.W,
            padx=6, pady=(2, 4))

        monitor = ttk.Frame(frame)
        monitor.grid(row=len(labels) + 3, column=0, columnspan=2, sticky=tk.EW,
                     padx=6, pady=(0, 6))
        ttk.Checkbutton(monitor, text=self._tr("监测在线状态", "Monitor connection"),
                        variable=self.sensor_monitor_var,
                        command=self._toggle_sensor_monitor).pack(side=tk.LEFT)
        ttk.Label(monitor, text=self._tr("周期(s)：", "Interval (s):")).pack(
            side=tk.LEFT, padx=(10, 2))
        ttk.Entry(monitor, textvariable=self.sensor_monitor_interval_var,
                  width=6).pack(side=tk.LEFT)

    def _build_acquisition_panel(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text=self._tr("数据采集", "Acquisition"))
        frame.pack(fill=tk.X, pady=(0, 6))

        ttk.Button(frame, text=self._tr("读取环境光", "Read ambient"),
                   command=lambda: self.send_command("READ")).grid(
                       row=0, column=0, columnspan=2, sticky=tk.EW,
                       padx=6, pady=(6, 3))
        ttk.Button(frame, text=self._tr("四光源完整测量", "Measure all four LEDs"),
                   command=lambda: self.send_command("MEASURE")).grid(
                       row=1, column=0, columnspan=2, sticky=tk.EW,
                       padx=6, pady=3)

        ttk.Label(frame, text=self._tr("连续周期 (ms)：", "Stream interval (ms):")).grid(
            row=2, column=0, sticky=tk.E, padx=(6, 3), pady=(7, 3))
        ttk.Entry(frame, textvariable=self.stream_interval_var, width=10).grid(
            row=2, column=1, sticky=tk.EW, padx=(3, 6), pady=(7, 3))
        self.stream_button = ttk.Button(frame, text=self._tr("开始连续读取", "Start streaming"),
                                        command=self.toggle_stream)
        self.stream_button.grid(row=3, column=0, columnspan=2, sticky=tk.EW,
                                padx=6, pady=3)
        ttk.Button(frame, text=self._tr("停止采集", "Stop acquisition"),
                   command=lambda: self.send_command("STOP")).grid(
                       row=4, column=0, sticky=tk.EW, padx=(6, 3), pady=(3, 6))
        ttk.Button(frame, text=self._tr("清空图表", "Clear charts"),
                   command=self.clear_measurements).grid(
                       row=4, column=1, sticky=tk.EW, padx=(3, 6), pady=(3, 6))
        frame.columnconfigure(0, weight=1)
        frame.columnconfigure(1, weight=1)

    def _build_io_tab(self) -> None:
        pane = ttk.Panedwindow(self.io_tab, orient=tk.HORIZONTAL)
        pane.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)

        left = ttk.Frame(pane)
        right = ttk.Frame(pane)
        pane.add(left, weight=1)
        pane.add(right, weight=2)

        led_frame = ttk.LabelFrame(
            left, text=self._tr("四路 LED 独立控制", "Four-channel LED control"))
        led_frame.pack(fill=tk.X, pady=(0, 8))
        for row, light in enumerate(LIGHTS):
            ttk.Label(led_frame, text=self._light_label(light), width=12).grid(
                row=row, column=0, padx=8, pady=5, sticky=tk.W)
            ttk.Label(led_frame, textvariable=self.led_vars[light], width=8).grid(
                row=row, column=1, padx=4)
            ttk.Button(led_frame, text=self._tr("开", "On"),
                       command=lambda x=light: self.send_command(f"LED {x} ON")).grid(
                row=row, column=2, padx=3)
            ttk.Button(led_frame, text=self._tr("关", "Off"),
                       command=lambda x=light: self.send_command(f"LED {x} OFF")).grid(
                row=row, column=3, padx=3)
        ttk.Label(led_frame, text=self._tr("掩码：", "Mask:")).grid(
            row=4, column=0, padx=8, pady=6, sticky=tk.E)
        ttk.Label(led_frame, textvariable=self.led_mask_var).grid(row=4, column=1, sticky=tk.W)
        ttk.Button(led_frame, text=self._tr("全部关闭", "All off"),
                   command=lambda: self.send_command("LED ALL OFF")).grid(row=5, column=0, columnspan=2, padx=6, pady=8)
        ttk.Button(led_frame, text=self._tr("循环测试", "Cycle test"),
                   command=lambda: self.send_command("LED CYCLE 500")).grid(row=5, column=2, columnspan=2, padx=6, pady=8)
        ttk.Button(led_frame, text=self._tr("读取 LED 状态", "Read LED status"),
                   command=lambda: self.send_command("LED STATUS")).grid(row=6, column=0, columnspan=4, pady=(0, 8))

        temp_frame = ttk.LabelFrame(left, text=self._tr("测温电阻/NTC", "Thermistor / NTC"))
        temp_frame.pack(fill=tk.X)
        temp_items = [
            (self._tr("状态", "Status"), self.temp_status_var),
            (self._tr("ADC 原始值", "Raw ADC"), self.temp_raw_var),
            (self._tr("电压", "Voltage"), self.temp_mv_var),
            (self._tr("电阻", "Resistance"), self.temp_resistance_var),
            (self._tr("温度", "Temperature"), self.temp_c_var),
        ]
        for row, (label, var) in enumerate(temp_items):
            ttk.Label(temp_frame, text=f"{label}：").grid(row=row, column=0, sticky=tk.E,
                                                        padx=8, pady=4)
            ttk.Label(temp_frame, textvariable=var).grid(row=row, column=1, sticky=tk.W,
                                                         padx=4, pady=4)
        ttk.Button(temp_frame, text=self._tr("读取温度", "Read temperature"),
                   command=lambda: self.send_command("TEMP")).grid(row=5, column=0, columnspan=2, pady=6)
        monitor = ttk.Frame(temp_frame)
        monitor.grid(row=6, column=0, columnspan=2, pady=(0, 8))
        ttk.Checkbutton(monitor, text=self._tr("连续监测", "Continuous monitoring"),
                        variable=self.temp_monitor_var,
                        command=self._toggle_temperature_monitor).pack(side=tk.LEFT)
        ttk.Label(monitor, text=self._tr("周期(s)：", "Interval (s):")).pack(
            side=tk.LEFT, padx=(8, 2))
        ttk.Entry(monitor, textvariable=self.temp_interval_var, width=6).pack(side=tk.LEFT)

        plot_frame = ttk.LabelFrame(right, text=self._tr("温度变化", "Temperature history"))
        plot_frame.pack(fill=tk.BOTH, expand=True)
        self.temp_figure = Figure(figsize=(8, 5), dpi=100)
        self.temp_ax = self.temp_figure.add_subplot(111)
        self.temp_canvas = FigureCanvasTkAgg(self.temp_figure, master=plot_frame)
        self.temp_canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

    def _build_test_tab(self) -> None:
        details = ttk.LabelFrame(
            self.test_tab, text=self._tr("设备详细信息", "Device details"))
        details.pack(fill=tk.X, padx=8, pady=(8, 0))
        detail_items = [
            (self._tr("状态", "Status"), "status"),
            (self._tr("协议族", "Protocol family"), "family"),
            (self._tr("可能型号", "Possible models"), "candidates"),
            (self._tr("寄存器协议", "Register protocol"), "protocol"),
            (self._tr("请求配置", "Requested profile"), "profile"),
            (self._tr("实际配置", "Effective profile"), "effective_profile"),
            (self._tr("配置来源", "Profile source"), "profile_ambiguous"),
            (self._tr("置信度", "Confidence"), "confidence"),
            (self._tr("I²C 地址", "I²C address"), "address"),
            (self._tr("原始 ID", "Raw ID"), "id_raw"),
            (self._tr("ID 编码", "ID code"), "id_code"),
            (self._tr("版本 / 辅助", "Revision / auxiliary"), "revision_aux"),
            (self._tr("签名寄存器", "Signature registers"), "signatures"),
            (self._tr("通道数", "Channels"), "channel_count"),
            (self._tr("固件 / 协议", "Firmware / protocol"), "firmware_protocol"),
        ]
        for index, (title, key) in enumerate(detail_items):
            row = index // 3
            group = index % 3
            ttk.Label(details, text=f"{title}：").grid(
                row=row, column=group * 2, sticky=tk.E,
                padx=(8, 3), pady=2)
            ttk.Label(details, textvariable=self.identity_display_vars[key]).grid(
                row=row, column=group * 2 + 1, sticky=tk.W,
                padx=(0, 8), pady=2)
            details.columnconfigure(group * 2 + 1, weight=1)

        top = ttk.Frame(self.test_tab)
        top.pack(fill=tk.X, padx=8, pady=8)

        config = ttk.LabelFrame(top, text=self._tr("采集参数", "Acquisition parameters"))
        config.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(0, 4))
        ttk.Checkbutton(config, text=self._tr("自动增益", "Automatic gain"),
                        variable=self.autogain_var,
                        command=self.set_autogain).grid(row=0, column=0, columnspan=2,
                                                        padx=8, pady=6, sticky=tk.W)
        ttk.Label(config, text=self._tr("固定增益：", "Fixed gain:")).grid(
            row=1, column=0, sticky=tk.E, padx=8, pady=4)
        self.gain_combo = ttk.Combobox(config, textvariable=self.gain_var,
                                       values=GAIN_LABELS, state="readonly", width=12)
        self.gain_combo.grid(row=1, column=1, sticky=tk.W, pady=4)
        ttk.Button(config, text=self._tr("设置增益", "Set gain"),
                   command=self.set_gain).grid(row=1, column=2, padx=8)
        ttk.Label(config, text="ATIME：").grid(row=2, column=0, sticky=tk.E, padx=8, pady=4)
        ttk.Entry(config, textvariable=self.atime_var, width=12).grid(row=2, column=1, sticky=tk.W)
        ttk.Button(config, text=self._tr("设置 ATIME", "Set ATIME"),
                   command=self.set_atime).grid(row=2, column=2, padx=8)
        ttk.Label(config, text="ASTEP：").grid(row=3, column=0, sticky=tk.E, padx=8, pady=4)
        ttk.Entry(config, textvariable=self.astep_var, width=12).grid(row=3, column=1, sticky=tk.W)
        ttk.Button(config, text=self._tr("设置 ASTEP", "Set ASTEP"),
                   command=self.set_astep).grid(row=3, column=2, padx=8)

        tests = ttk.LabelFrame(top, text=self._tr("快速测试", "Quick tests"))
        tests.pack(side=tk.RIGHT, fill=tk.BOTH, expand=True, padx=(4, 0))
        test_buttons = [
            ("PING", "PING"), (self._tr("主板状态", "Board status"), "BOARD"),
            (self._tr("传感器配置", "Sensor configuration"), "AS CONFIG"),
            (self._tr("寄存器读写自检", "Register R/W test"), "AS RWTEST"),
            (self._tr("强制重初始化", "Force reinitialization"), "AS FORCEINIT"),
            (self._tr("传感器复位", "Sensor reset"), "AS RESET"),
            (self._tr("强制采样", "Force sample"), "AS SAMPLE FORCE"),
            (self._tr("完整诊断", "Full diagnostics"), "DIAG"),
        ]
        for idx, (text, command) in enumerate(test_buttons):
            ttk.Button(tests, text=text,
                       command=lambda c=command: self.send_command(c)).grid(
                row=idx // 2, column=idx % 2, padx=8, pady=5, sticky=tk.EW)
        tests.columnconfigure(0, weight=1)
        tests.columnconfigure(1, weight=1)

        reg = ttk.LabelFrame(
            self.test_tab,
            text=self._tr("通用寄存器访问（谨慎写入）", "Register access (write with care)"))
        reg.pack(fill=tk.X, padx=8, pady=(0, 8))
        ttk.Label(reg, text="Bank：").pack(side=tk.LEFT, padx=(8, 2), pady=8)
        ttk.Combobox(reg, textvariable=self.reg_bank_var, values=("0", "1"),
                     state="readonly", width=5).pack(side=tk.LEFT, padx=2)
        ttk.Label(reg, text=self._tr("地址：", "Address:")).pack(side=tk.LEFT, padx=(12, 2))
        ttk.Entry(reg, textvariable=self.reg_addr_var, width=10).pack(side=tk.LEFT)
        ttk.Label(reg, text=self._tr("写入值：", "Value:")).pack(side=tk.LEFT, padx=(12, 2))
        ttk.Entry(reg, textvariable=self.reg_value_var, width=10).pack(side=tk.LEFT)
        ttk.Button(reg, text=self._tr("读取", "Read"),
                   command=self.read_register).pack(side=tk.LEFT, padx=8)
        ttk.Button(reg, text=self._tr("写入并回读", "Write and verify"),
                   command=self.write_register).pack(side=tk.LEFT, padx=4)
        ttk.Button(reg, text=self._tr("转储 16 个寄存器", "Dump 16 registers"),
                   command=self.dump_registers).pack(side=tk.LEFT, padx=8)

        raw = ttk.LabelFrame(self.test_tab, text=self._tr("原始命令", "Raw command"))
        raw.pack(fill=tk.X, padx=8, pady=(0, 8))
        entry = ttk.Entry(raw, textvariable=self.raw_command_var)
        entry.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=8, pady=8)
        entry.bind("<Return>", lambda _event: self.send_raw_command())
        ttk.Button(raw, text=self._tr("发送", "Send"),
                   command=self.send_raw_command).pack(side=tk.RIGHT, padx=8)

        note = self._tr(
            "自动识别以 I²C 地址、只读 ID 和寄存器签名确定协议族。"
            "AS7341/AS7341L 共用 ID；AS7343/AS7343L 也共用地址和 ID，"
            "所以无法仅靠数字通信证明精确料号。AS7343 与 AS7343L 的第三轮自动 SMUX "
            "槽位顺序不同，可在“数据解释配置”中手动选择；该选择只改变通道解释，"
            "不会把手动选择冒充为硬件识别结果。",
            "Auto-detection uses the I²C address, read-only ID and register signatures to "
            "identify the protocol family. AS7341/AS7341L share one ID, while AS7343/AS7343L "
            "also share an address and ID, so digital communication alone cannot prove the "
            "exact orderable part. The manual profile changes channel interpretation only; "
            "it is never presented as new hardware-identification evidence.")
        ttk.Label(self.test_tab, text=note, wraplength=1100).pack(fill=tk.X, padx=12, pady=6)

    def _build_log_tab(self) -> None:
        toolbar = ttk.Frame(self.log_tab)
        toolbar.pack(fill=tk.X, padx=8, pady=8)
        ttk.Button(toolbar, text=self._tr("清空日志", "Clear log"),
                   command=self.clear_log).pack(side=tk.LEFT)
        ttk.Button(toolbar, text=self._tr("保存日志", "Save log"),
                   command=self.save_log).pack(side=tk.LEFT, padx=6)
        ttk.Button(toolbar, text=self._tr("复制全部", "Copy all"),
                   command=lambda: self.root.clipboard_append(self.log_text.get("1.0", tk.END))).pack(side=tk.LEFT)

        self.log_text = tk.Text(self.log_tab, wrap=tk.NONE, font=("Consolas", 10))
        y_scroll = ttk.Scrollbar(self.log_tab, orient=tk.VERTICAL, command=self.log_text.yview)
        x_scroll = ttk.Scrollbar(self.log_tab, orient=tk.HORIZONTAL, command=self.log_text.xview)
        self.log_text.configure(yscrollcommand=y_scroll.set, xscrollcommand=x_scroll.set)
        self.log_text.pack(side=tk.LEFT, fill=tk.BOTH, expand=True, padx=(8, 0), pady=(0, 8))
        y_scroll.pack(side=tk.RIGHT, fill=tk.Y, padx=(0, 8), pady=(0, 8))
        x_scroll.place(relx=0.005, rely=0.975, relwidth=0.975)

    # ---------- 串口与日志 ----------

    def refresh_ports(self) -> None:
        ports = list(serial.tools.list_ports.comports())
        values = [f"{p.device} — {p.description}" for p in ports]
        self.port_combo["values"] = values
        if values:
            current_device = self._selected_port_device()
            match = next((v for v in values if v.startswith(current_device + " ")), None)
            self.port_var.set(match or values[0])
        else:
            self.port_var.set("")

    def _selected_port_device(self) -> str:
        return self.port_var.get().split(" — ", 1)[0].strip()

    def toggle_connection(self) -> None:
        if self.serial.connected:
            self.disconnect()
        else:
            self.connect()

    def connect(self) -> None:
        port_name = self._selected_port_device()
        if not port_name:
            messagebox.showwarning(
                self._tr("串口", "Serial port"),
                self._tr("没有可用串口，请连接设备后刷新。",
                         "No serial port is available. Connect the device and refresh."))
            return
        try:
            baud = int(self.baud_var.get())
            self.serial.connect(port_name, baud)
        except (ValueError, serial.SerialException, OSError) as exc:
            messagebox.showerror(self._tr("连接失败", "Connection failed"), str(exc))
            return

        self.connection_var.set(self._tr(
            f"已连接：{port_name} @ {baud}", f"Connected: {port_name} @ {baud}"))
        self.connect_button.configure(text=self._tr("断开", "Disconnect"))
        self.log("SYS", self._tr(
            f"串口已连接：{port_name} @ {baud}",
            f"Serial port connected: {port_name} @ {baud}"))
        self.root.after(200, lambda: self.send_command("PING"))
        self.root.after(400, lambda: self.send_command("DETECT"))
        self.root.after(850, lambda: self.send_command("TEMP"))
        self.root.after(1050, lambda: self.send_command("LED STATUS"))

    def disconnect(self) -> None:
        self.serial.disconnect()
        self.connection_var.set(self._tr("未连接", "Disconnected"))
        self.connect_button.configure(text=self._tr("连接", "Connect"))
        self.stream_active = False
        self.stream_button.configure(text=self._tr("开始连续读取", "Start streaming"))
        self.identity.status = "DISCONNECTED"
        self._update_identity_display()
        self.log("SYS", self._tr("串口已断开", "Serial port disconnected"))

    def send_command(self, command: str, quiet: bool = False) -> None:
        try:
            self.serial.send(command)
            if not quiet:
                self.log("TX", command)
        except (serial.SerialException, UnicodeEncodeError, OSError) as exc:
            if not quiet:
                self.log("ERR", self._tr(f"发送失败：{exc}", f"Send failed: {exc}"))
            if not self.serial.connected:
                self.disconnect()

    def send_raw_command(self) -> None:
        command = self.raw_command_var.get().strip()
        if command:
            self.send_command(command)
            self.raw_command_var.set("")

    def log(self, level: str, text: str) -> None:
        timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        line = f"[{timestamp}] {level:<4} {text}\n"
        self.log_text.insert(tk.END, line)
        self.log_text.see(tk.END)

    def clear_log(self) -> None:
        self.log_text.delete("1.0", tk.END)

    def save_log(self) -> None:
        path = filedialog.asksaveasfilename(
            title=self._tr("保存通信日志", "Save communication log"),
            defaultextension=".txt",
            filetypes=[(self._tr("文本文件", "Text files"), "*.txt"),
                       (self._tr("所有文件", "All files"), "*.*")],
            initialfile=f"ams_spectral_log_{datetime.now():%Y%m%d_%H%M%S}.txt",
        )
        if path:
            Path(path).write_text(self.log_text.get("1.0", tk.END), encoding="utf-8")

    def _process_events(self) -> None:
        try:
            while True:
                kind, data = self.events.get_nowait()
                if kind == "line":
                    self._handle_serial_line(str(data))
                elif kind == "serial_error":
                    self.log("ERR", self._tr(f"串口异常：{data}", f"Serial error: {data}"))
                    self.disconnect()
        except queue.Empty:
            pass
        self.root.after(50, self._process_events)

    # ---------- 协议解析 ----------

    @staticmethod
    def _crc16_ccitt(data: bytes) -> int:
        crc = 0xFFFF
        for byte in data:
            crc ^= byte << 8
            for _ in range(8):
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
        return crc

    def _unwrap_frame(self, line: str) -> tuple[str, Optional[bool]]:
        if not line.startswith("$"):
            return line, None
        match = re.fullmatch(r"\$(.*)\*([0-9A-Fa-f]{4})", line)
        if not match:
            return line, False
        payload = match.group(1)
        received = int(match.group(2), 16)
        calculated = self._crc16_ccitt(payload.encode("utf-8"))
        return payload, received == calculated

    @staticmethod
    def _kv(tokens: list[str]) -> dict[str, str]:
        result: dict[str, str] = {}
        for token in tokens:
            if "=" in token:
                key, value = token.split("=", 1)
                result[key.strip().upper()] = value.strip()
        return result

    @staticmethod
    def _parse_int(text: str, default: int = 0, base: int = 10) -> int:
        try:
            return int(text, base)
        except (TypeError, ValueError):
            return default

    def _handle_serial_line(self, raw_line: str) -> None:
        payload, crc_ok = self._unwrap_frame(raw_line)
        if crc_ok is True:
            self.crc_state = True
            self.crc_status_var.set(self._tr("CRC：正常", "CRC: OK"))
        elif crc_ok is False:
            self.crc_state = False
            self.crc_status_var.set(self._tr("CRC：错误", "CRC: error"))
            self.log("WARN", self._tr(
                f"CRC 校验失败：{raw_line}", f"CRC check failed: {raw_line}"))
            return

        self.log("RX", payload)
        tokens = payload.split(",")
        message = tokens[0].upper() if tokens else ""
        try:
            if message == "INFO":
                self._handle_info(self._kv(tokens[1:]))
            elif message == "SENSOR":
                self._handle_sensor(self._kv(tokens[1:]))
            elif message == "CHANNELS":
                self._handle_channels(tokens[1:])
            elif message == "AMBIENT":
                self._handle_ambient(tokens)
            elif message == "BEGIN" and len(tokens) > 1:
                self._begin_measurement(self._parse_int(tokens[1]))
            elif message == "MEAS":
                self._handle_measurement(tokens)
            elif message == "TEMP":
                self._handle_temperature(self._kv(tokens[1:]))
            elif message == "LEDSTAT":
                self._handle_led_status(self._kv(tokens[1:]))
            elif message == "ACK" and len(tokens) >= 2:
                if tokens[1].upper() == "STREAM":
                    self.stream_active = True
                    self.stream_button.configure(
                        text=self._tr("停止连续读取", "Stop streaming"))
                elif tokens[1].upper() == "STOP":
                    self.stream_active = False
                    self.stream_button.configure(
                        text=self._tr("开始连续读取", "Start streaming"))
                elif tokens[1].upper() == "PROFILE":
                    kv = self._kv(tokens[2:])
                    if kv.get("STATUS") == "OK":
                        self.last_frame_var.set(
                            self._tr(
                                f"数据解释配置已切换：{kv.get('EFFECTIVE', self.profile_var.get())}",
                                f"Data profile changed: {kv.get('EFFECTIVE', self.profile_var.get())}"))
                    else:
                        self.last_frame_var.set(
                            self._tr(
                                f"配置切换失败：{kv.get('STATUS', 'UNKNOWN')}",
                                f"Profile change failed: {kv.get('STATUS', 'UNKNOWN')}"))
            elif message == "ERR":
                prefix = self._tr("设备错误：", "Device error: ")
                self.last_frame_var.set(prefix + ",".join(tokens[1:]))
            elif message == "END" and len(tokens) > 1:
                self._finish_measurement(self._parse_int(tokens[1]))
        except (IndexError, ValueError) as exc:
            self.log("WARN", self._tr(
                f"报文解析失败：{exc}；原文={payload}",
                f"Frame parsing failed: {exc}; payload={payload}"))

    def _handle_info(self, kv: dict[str, str]) -> None:
        self.identity.firmware = kv.get("FW", self.identity.firmware)
        self.identity.protocol_version = kv.get("PROTO", self.identity.protocol_version)
        self.identity.status = kv.get("SENSOR_STATUS", self.identity.status)
        self.identity.family = kv.get("FAMILY", self.identity.family)
        self.identity.candidates = kv.get("CANDIDATES", self.identity.candidates)
        self.identity.protocol = kv.get("SENSOR_PROTOCOL", self.identity.protocol)
        self.identity.profile = kv.get("PROFILE", self.identity.profile)
        self.identity.effective_profile = kv.get(
            "EFFECTIVE_PROFILE", self.identity.effective_profile)
        self.identity.profile_ambiguous = kv.get(
            "PROFILE_AMBIGUOUS", self.identity.profile_ambiguous)
        self.identity.confidence = kv.get("CONFIDENCE", self.identity.confidence)
        self.identity.address = self._hex_label(kv.get("ADDR", self.identity.address))
        self.identity.id_raw = self._hex_label(kv.get("ID", self.identity.id_raw))
        self.identity.id_code = self._hex_label(kv.get("ID_CODE", self.identity.id_code))
        self.identity.revision = self._hex_label(kv.get("REV", self.identity.revision))
        self.identity.auxiliary = self._hex_label(kv.get("AUX", self.identity.auxiliary))
        self.identity.channel_count = kv.get("CHANNEL_COUNT", self.identity.channel_count)

        self.autogain_var.set(kv.get("AUTOGAIN", "1") == "1")
        gain_index = self._parse_int(kv.get("GAIN", "5"), 5)
        if 0 <= gain_index < len(GAIN_LABELS):
            self.gain_var.set(GAIN_LABELS[gain_index])
        self.atime_var.set(kv.get("ATIME", self.atime_var.get()))
        self.astep_var.set(kv.get("ASTEP", self.astep_var.get()))
        self._update_identity_display()
        self._update_gain_options()
        self._update_profile_options()

    def _handle_sensor(self, kv: dict[str, str]) -> None:
        self.identity.status = kv.get("STATUS", self.identity.status)
        self.identity.family = kv.get("FAMILY", self.identity.family)
        self.identity.candidates = kv.get("CANDIDATES", self.identity.candidates)
        self.identity.protocol = kv.get("PROTOCOL", self.identity.protocol)
        self.identity.profile = kv.get("PROFILE", self.identity.profile)
        self.identity.effective_profile = kv.get(
            "EFFECTIVE_PROFILE", self.identity.effective_profile)
        self.identity.profile_ambiguous = kv.get(
            "PROFILE_AMBIGUOUS", self.identity.profile_ambiguous)
        self.identity.confidence = kv.get("CONFIDENCE", self.identity.confidence)
        self.identity.address = self._hex_label(kv.get("ADDR", self.identity.address))
        self.identity.id_raw = self._hex_label(kv.get("ID_RAW", self.identity.id_raw))
        self.identity.id_code = self._hex_label(kv.get("ID_CODE", self.identity.id_code))
        self.identity.revision = self._hex_label(kv.get("REV", self.identity.revision))
        self.identity.auxiliary = self._hex_label(kv.get("AUX", self.identity.auxiliary))
        self.identity.sig92 = self._hex_label(kv.get("SIG92", self.identity.sig92))
        self.identity.sig5a = self._hex_label(kv.get("SIG5A", self.identity.sig5a))
        self.identity.sigcfg0 = self._hex_label(kv.get("SIGCFG0", self.identity.sigcfg0))
        self.identity.sigd6 = self._hex_label(kv.get("SIGD6", self.identity.sigd6))
        self.identity.channel_count = kv.get("CHANNEL_COUNT", self.identity.channel_count)
        self._update_identity_display()
        self._update_gain_options()
        self._update_profile_options()

        fingerprint = self.identity.fingerprint()
        if self.last_identity_fingerprint is not None and fingerprint != self.last_identity_fingerprint:
            prefix = self._tr("传感器识别结果发生变化：", "Sensor identity changed: ")
            self.log("WARN", prefix + " / ".join(fingerprint))
        self.last_identity_fingerprint = fingerprint

    @staticmethod
    def _hex_label(value: str) -> str:
        value = str(value).strip()
        if not value or value == "-":
            return "-"
        value = value[2:] if value.lower().startswith("0x") else value
        return "0x" + value.upper()

    def _update_identity_display(self) -> None:
        values = {
            "status": self._status_label(self.identity.status),
            "family": self.identity.family,
            "candidates": self.identity.candidates.replace("_OR_", " / ").replace("_", " "),
            "protocol": self.identity.protocol,
            "profile": self.identity.profile,
            "effective_profile": self.identity.effective_profile,
            "profile_ambiguous": (
                self._tr("自动默认/硬件仍有歧义", "AUTO default / hardware remains ambiguous")
                if self.identity.profile_ambiguous == "1"
                else self._tr("手动指定或地址可唯一确认",
                              "Manual profile or unique address evidence")),
            "confidence": self.identity.confidence,
            "address": self.identity.address,
            "id_raw": self.identity.id_raw,
            "id_code": self.identity.id_code,
            "revision_aux": f"REV={self.identity.revision}, AUX={self.identity.auxiliary}",
            "signatures": (f"0x92={self.identity.sig92}, 0x5A={self.identity.sig5a}, "
                           f"CFG0={self.identity.sigcfg0}, D6={self.identity.sigd6}"),
            "channel_count": self.identity.channel_count,
            "firmware_protocol": f"{self.identity.firmware} / {self.identity.protocol_version}",
        }
        for key, value in values.items():
            self.identity_display_vars[key].set(value)

    def _update_profile_options(self) -> None:
        family = self.identity.family
        if family == "AS7341_FAMILY":
            values = ("AUTO", "AS7341", "AS7341L")
        elif family == "AS7343_FAMILY":
            values = ("AUTO", "AS7343", "AS7343L")
        elif family == "TCS3448":
            values = ("AUTO", "TCS3448")
        else:
            values = ("AUTO", "AS7341", "AS7341L", "AS7343", "AS7343L", "TCS3448")
        self.profile_combo["values"] = values
        requested = self.identity.profile if self.identity.profile in values else "AUTO"
        self.profile_var.set(requested)

        if self.identity.profile_ambiguous == "1":
            candidates = self.identity.candidates.replace("_OR_", " / ")
            self.profile_note_var.set(self._tr(
                f"数字通信只能确认 {candidates}；当前 AUTO 按 "
                f"{self.identity.effective_profile} 解释原始 ADC 槽位。"
                "购买标签或芯片丝印确认后，可手动选择对应配置。",
                f"Digital communication only narrows the device to {candidates}. "
                f"AUTO currently interprets the ADC slots as {self.identity.effective_profile}. "
                "Select a profile manually after checking the package marking or purchase label."))
        elif "_OR_" in self.identity.candidates:
            self.profile_note_var.set(self._tr(
                f"当前按 {self.identity.effective_profile} 手动解释数据；"
                "这不是新的硬件 ID 证据，候选型号仍以“可能型号”为准。",
                f"Data is manually interpreted as {self.identity.effective_profile}. "
                "This is not new hardware-ID evidence; the candidate list remains authoritative."))
        elif self.identity.family == "TCS3448":
            self.profile_note_var.set(self._tr(
                "0x59 地址与 0x81 ID 组合高度符合 TCS3448；"
                "已按 TCS3448 官方中心波长解释通道。",
                "The 0x59 address and 0x81 ID combination strongly matches TCS3448. "
                "Channels use the official TCS3448 center wavelengths."))
        else:
            self.profile_note_var.set(
                self._tr("等待有效识别结果。", "Waiting for a valid identity result."))

    def apply_profile(self) -> None:
        profile = self.profile_var.get().strip().upper()
        if not profile:
            return
        self.send_command(f"SET PROFILE {profile}")

    def _update_gain_options(self) -> None:
        if self.identity.family == "AS7341_FAMILY":
            values = GAIN_LABELS[:11]
        else:
            values = GAIN_LABELS
        self.gain_combo["values"] = values
        if self.gain_var.get() not in values:
            self.gain_var.set(values[-1])

    def _handle_channels(self, names: list[str]) -> None:
        names = [name.strip() for name in names if name.strip()]
        if not names or len(names) > 14:
            return
        if names != self.channels:
            self.channels = names
            self.ambient = [0] * len(names)
            self.measurements.clear()
            self.current_measurement_sequence = None
            self._rebuild_spectrum_table()
            self._redraw_spectrum_plot()
            self.log("SYS", self._tr(
                f"通道表已切换为 {len(names)} 通道：{', '.join(names)}",
                f"Channel map changed to {len(names)} channels: {', '.join(names)}"))

    def _begin_measurement(self, sequence: int) -> None:
        """开始一轮新测量时清除上一轮曲线，只保留本轮最新数据。"""
        self.current_measurement_sequence = sequence
        self.measurements.clear()
        self.ambient = [0] * len(self.channels)
        self._update_spectrum_table()
        self._redraw_spectrum_plot()
        self.last_frame_var.set(self._tr(
            f"开始测量序号 {sequence}，已清除上一轮曲线",
            f"Measurement {sequence} started; previous curves cleared"))

    def _handle_ambient(self, tokens: list[str]) -> None:
        count = len(self.channels)
        if len(tokens) < 8 + count:
            raise ValueError(self._tr(
                f"AMBIENT 数据不足，期望 {count} 个通道",
                f"AMBIENT frame is short; expected {count} channels"))
        values = [self._parse_int(x) for x in tokens[8:8 + count]]
        # 环境光采集是一幅独立的最新快照，不与之前的 LED 测量曲线叠加。
        self.current_measurement_sequence = None
        self.measurements.clear()
        self.ambient = values
        self.last_frame_var.set(self._tr(
            f"环境光：gain={tokens[2]}，ATIME={tokens[4]}，ASTEP={tokens[5]}，flags={tokens[7]}",
            f"Ambient: gain={tokens[2]}, ATIME={tokens[4]}, ASTEP={tokens[5]}, flags={tokens[7]}"))
        self._update_spectrum_table()
        self._redraw_spectrum_plot()

    def _handle_measurement(self, tokens: list[str]) -> None:
        count = len(self.channels)
        expected = 11 + 2 * count
        if len(tokens) < expected:
            raise ValueError(self._tr(
                f"MEAS 数据不足，期望至少 {expected} 项，实际 {len(tokens)}",
                f"MEAS frame is short; expected at least {expected} fields, got {len(tokens)}"))
        source = tokens[2].upper()
        frame = SpectrumFrame(
            sequence=self._parse_int(tokens[1]),
            source=source,
            gain_index=self._parse_int(tokens[3]),
            gain_x1000=self._parse_int(tokens[4]),
            atime=self._parse_int(tokens[5]),
            astep=self._parse_int(tokens[6]),
            tint_us=self._parse_int(tokens[7]),
            temperature_x10=self._parse_int(tokens[8]),
            light_flags=self._parse_int(tokens[9], base=16),
            dark_flags=self._parse_int(tokens[10], base=16),
            light=[self._parse_int(x) for x in tokens[11:11 + count]],
            dark=[self._parse_int(x) for x in tokens[11 + count:11 + 2 * count]],
        )
        # 固件正常会先发送 BEGIN；即使 BEGIN 丢失，也按 sequence 自动切换到最新批次。
        if self.current_measurement_sequence != frame.sequence:
            self.current_measurement_sequence = frame.sequence
            self.measurements.clear()
            self.ambient = [0] * count
        self.measurements[source] = frame
        source_label = self._light_label(source) if source in LIGHTS else source
        self.last_frame_var.set(self._tr(
            f"测量序号 {frame.sequence}，光源 {source_label}，gain={frame.gain_index}，"
            f"积分={frame.tint_us / 1000:.2f} ms，温度={frame.temperature_x10 / 10:.1f} °C",
            f"Measurement {frame.sequence}, source {source_label}, gain={frame.gain_index}, "
            f"integration={frame.tint_us / 1000:.2f} ms, temperature={frame.temperature_x10 / 10:.1f} °C"))
        self._update_spectrum_table()
        self._redraw_spectrum_plot()

    def _finish_measurement(self, sequence: int) -> None:
        """保存一轮四光源的派生指标，用于重复性和漂移分析。"""
        if self.measurements:
            snapshot: dict[str, Any] = {"sequence": sequence, "time": time.time()}
            spectral_indices = [item[0] for item in self._spectral_channel_layout()]
            for light in LIGHTS:
                frame = self.measurements.get(light)
                if frame:
                    gain = max(frame.gain_x1000 / 1000.0, 0.5)
                    spectral_net = [self._safe_at(frame.net, idx)
                                    for idx in spectral_indices]
                    snapshot[light] = {
                        "sum_net": float(sum(spectral_net)),
                        "sum_net_1x": float(sum(spectral_net)) / gain,
                        "peak": float(max(spectral_net, default=0)),
                        "gain": frame.gain_index,
                        "flags": frame.light_flags | frame.dark_flags,
                        "temperature": frame.temperature_x10 / 10.0,
                    }
            if len(snapshot) > 2:
                if (not self.measurement_history or
                        self.measurement_history[-1].get("sequence") != sequence):
                    self.measurement_history.append(snapshot)
        self.last_frame_var.set(self._tr(
            f"完整测量完成，序号 {sequence}｜稳定性记录 {len(self.measurement_history)} 组",
            f"Full measurement {sequence} complete | {len(self.measurement_history)} stability records"))
        self._redraw_stability_plot()

    def _handle_temperature(self, kv: dict[str, str]) -> None:
        status = kv.get("STATUS", "UNKNOWN")
        self.temp_status_code = status
        self.temp_status_var.set(self._status_label(status))
        self.temp_raw_var.set(kv.get("RAW", "-"))
        mv = kv.get("MV", "-")
        self.temp_mv_var.set(f"{mv} mV" if mv != "-" else "-")
        resistance = kv.get("R_OHM", "-")
        self.temp_resistance_var.set(f"{resistance} Ω" if resistance != "-" else "-")
        if "T_X10" in kv:
            temp_c = self._parse_int(kv["T_X10"]) / 10.0
            self.temp_c_var.set(f"{temp_c:.1f} °C")
            self.temperature_history.append((time.monotonic(), temp_c))
            self._redraw_temperature_plot()
        else:
            self.temp_c_var.set("-")

    def _handle_led_status(self, kv: dict[str, str]) -> None:
        self.led_mask_var.set(kv.get("MASK", "--"))
        for light in LIGHTS:
            self.led_states[light] = kv.get(light, "0") == "1"
            self.led_vars[light].set(
                self._tr("开启", "On") if self.led_states[light]
                else self._tr("关闭", "Off"))

    # ---------- 数据展示 ----------

    def _rebuild_spectrum_table(self) -> None:
        for item in self.spectrum_tree.get_children():
            self.spectrum_tree.delete(item)
        for name in self.channels:
            self.spectrum_tree.insert("", tk.END, iid=name,
                                      values=(name, 0, 0, 0, 0, 0, 0))
        self._rebuild_value_matrix()
        self._update_spectrum_table()

    def _rebuild_value_matrix(self) -> None:
        """按“数据源为行、传感器通道为列”建立同屏实际值矩阵。"""
        columns = ("source",) + tuple(
            f"channel_{index}" for index in range(len(self.channels)))
        self.value_matrix.configure(columns=columns, displaycolumns=columns)
        self.value_matrix.heading("source", text=self._tr("数据源", "Source"))
        self.value_matrix.column("source", width=82, minwidth=74,
                                 anchor=tk.CENTER, stretch=False)
        for index, name in enumerate(self.channels):
            column = f"channel_{index}"
            self.value_matrix.heading(column, text=name)
            self.value_matrix.column(column, width=68, minwidth=55,
                                     anchor=tk.CENTER, stretch=True)

        for item in self.value_matrix.get_children():
            self.value_matrix.delete(item)
        self.value_matrix.insert("", tk.END, iid="matrix_ambient")
        for light in LIGHTS:
            self.value_matrix.insert("", tk.END, iid=f"matrix_{light}")

    def _update_value_matrix(self) -> None:
        count = len(self.channels)
        ambient_values = [self._safe_at(self.ambient, index)
                          for index in range(count)]
        if self.value_matrix.exists("matrix_ambient"):
            self.value_matrix.item(
                "matrix_ambient",
                values=[self._tr("环境光", "Ambient")] + ambient_values)

        for light in LIGHTS:
            frame = self.measurements.get(light)
            channel_values: list[int | str]
            if frame is None:
                channel_values = ["—"] * count
            else:
                channel_values = [self._safe_at(frame.net, index)
                                  for index in range(count)]
            item = f"matrix_{light}"
            if self.value_matrix.exists(item):
                self.value_matrix.item(
                    item,
                    values=[self._tr(
                        self._light_label(light) + " 净值",
                        self._light_label(light) + " net")] + channel_values)

    def _update_spectrum_table(self) -> None:
        count = len(self.channels)
        dark = [0] * count
        if self.measurements:
            latest = max(self.measurements.values(), key=lambda x: (x.sequence, x.source))
            dark = latest.dark
        for idx, name in enumerate(self.channels):
            values = [name, self._safe_at(self.ambient, idx), self._safe_at(dark, idx)]
            for light in LIGHTS:
                frame = self.measurements.get(light)
                values.append(self._safe_at(frame.net, idx) if frame else 0)
            if self.spectrum_tree.exists(name):
                self.spectrum_tree.item(name, values=values)
        self._update_value_matrix()

    @staticmethod
    def _safe_at(values: list[int], index: int) -> int:
        return values[index] if 0 <= index < len(values) else 0

    @staticmethod
    def _channel_peak_nm(name: str) -> Optional[int]:
        """从固件通道标签提取典型峰值；CLEAR/FD_RAW 返回 None。"""
        match = re.search(r"_(\d+)$", name.upper())
        return int(match.group(1)) if match else None

    def _spectral_channel_layout(self) -> list[tuple[int, int, str]]:
        """返回真正具有单一典型峰值的滤光通道。"""
        result: list[tuple[int, int, str]] = []
        for index, name in enumerate(self.channels):
            peak_nm = self._channel_peak_nm(name)
            if peak_nm is not None:
                result.append((index, peak_nm, name))
        return result

    def _redraw_spectrum_plot(self) -> None:
        axes = (self.net_ax, self.normalized_ax, self.heatmap_ax, self.quality_ax)
        for ax in axes:
            ax.clear()
        positions = list(range(len(self.channels)))
        spectral_layout = self._spectral_channel_layout()
        spectral_indices = [item[0] for item in spectral_layout]
        spectral_x = [item[1] for item in spectral_layout]
        wavelength_ticks: list[int] = []
        if spectral_x:
            tick_start = (min(spectral_x) // 50) * 50
            tick_stop = ((max(spectral_x) + 49) // 50) * 50
            wavelength_ticks = list(range(tick_start, tick_stop + 1, 50))

        # 1. 仅将具有典型峰值的滤光通道绘制在真实 nm 横坐标上。
        ambient_spectral = [self._safe_at(self.ambient, idx)
                            for idx in spectral_indices]
        if any(ambient_spectral):
            self.net_ax.plot(spectral_x, ambient_spectral, marker="o",
                             label=self._tr("环境光", "Ambient"))
        for light in LIGHTS:
            frame = self.measurements.get(light)
            if frame:
                spectral_net = [self._safe_at(frame.net, idx)
                                for idx in spectral_indices]
                self.net_ax.plot(spectral_x, spectral_net, marker="o",
                                 label=self._tr(
                                     f"{self._light_label(light)} 净响应",
                                     f"{self._light_label(light)} net response"))
        self.net_ax.set_title(self._tr("暗场扣除后净响应", "Dark-subtracted response"))
        self.net_ax.set_ylabel(self._tr("ADC 计数", "ADC counts"))

        # 2. 每个光源归一化至自身峰值，用于比较谱形。
        for light in LIGHTS:
            frame = self.measurements.get(light)
            if frame:
                spectral_net = [self._safe_at(frame.net, idx)
                                for idx in spectral_indices]
                peak = max(spectral_net, default=0)
                normalized = [value / peak if peak > 0 else 0.0
                              for value in spectral_net]
                self.normalized_ax.plot(spectral_x, normalized, marker="o",
                                        label=self._light_label(light))
        self.normalized_ax.set_title(
            self._tr("峰值归一化谱形", "Peak-normalized spectral shape"))
        self.normalized_ax.set_ylabel(self._tr("相对响应 (0–1)", "Relative response (0–1)"))
        self.normalized_ax.set_ylim(-0.03, 1.08)

        # 3. 光源×通道热图，每行归一化以减少自动增益差异的影响。
        heat_rows: list[list[float]] = []
        heat_labels: list[str] = []
        for light in LIGHTS:
            frame = self.measurements.get(light)
            if frame:
                peak = max(frame.net, default=0)
                heat_rows.append([value / peak if peak > 0 else 0.0 for value in frame.net])
                heat_labels.append(self._light_label(light))
        if heat_rows:
            image = self.heatmap_ax.imshow(heat_rows, aspect="auto", vmin=0.0, vmax=1.0,
                                           interpolation="nearest", cmap="viridis")
            self.heatmap_ax.set_yticks(range(len(heat_labels)))
            self.heatmap_ax.set_yticklabels(heat_labels)
            self.heatmap_ax.set_title(
                self._tr("光源–通道归一化热图", "Normalized source-channel heatmap"))
            # 固定色标轴避免每次刷新累积 colorbar。
            image.set_clim(0.0, 1.0)
        else:
            self.heatmap_ax.set_title(
                self._tr("光源–通道归一化热图", "Normalized source-channel heatmap"))

        # 4. 信号质量：有效净信号占亮场读数的比例，不冒充统计 SNR。
        quality_labels: list[str] = []
        quality_values: list[float] = []
        quality_colors: list[str] = []
        for light in LIGHTS:
            frame = self.measurements.get(light)
            if frame:
                light_sum = sum(self._safe_at(frame.light, idx)
                                for idx in spectral_indices)
                net_sum = sum(self._safe_at(frame.net, idx)
                              for idx in spectral_indices)
                fraction = 100.0 * net_sum / light_sum if light_sum > 0 else 0.0
                quality_labels.append(self._light_label(light))
                quality_values.append(fraction)
                quality_colors.append("tab:red" if (frame.light_flags | frame.dark_flags) else "tab:blue")
        if quality_values:
            bars = self.quality_ax.bar(quality_labels, quality_values, color=quality_colors)
            for bar, value in zip(bars, quality_values):
                self.quality_ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height(),
                                     f"{value:.1f}%", ha="center", va="bottom", fontsize=8)
        self.quality_ax.set_title(self._tr(
            "有效净信号占亮场比例", "Usable net signal as a fraction of lit signal"))
        self.quality_ax.set_ylabel(self._tr("净信号 / 亮场 (%)", "Net / lit signal (%)"))

        for ax in axes:
            ax.grid(True, alpha=0.22)
        for ax in (self.net_ax, self.normalized_ax):
            ax.set_xlabel(self._tr("典型峰值波长 (nm)", "Nominal peak wavelength (nm)"))
            ax.set_xticks(wavelength_ticks)
            if spectral_x:
                ax.set_xlim(min(spectral_x) - 12, max(spectral_x) + 12)
        self.heatmap_ax.set_xlabel(self._tr(
            "传感器通道（含 CLEAR / FD_RAW）", "Sensor channels (including CLEAR / FD_RAW)"))
        self.heatmap_ax.set_xticks(positions)
        self.heatmap_ax.set_xticklabels(self.channels, rotation=45,
                                        ha="right", fontsize=7)
        self.quality_ax.set_xlabel(self._tr("激发光源", "Illumination source"))
        for ax in (self.net_ax, self.normalized_ax):
            if ax.lines:
                ax.legend(loc="best", fontsize=7)
        self.spectrum_figure.tight_layout(h_pad=2.4)
        self.spectrum_canvas.draw_idle()

    def _redraw_stability_plot(self) -> None:
        ax = self.stability_ax
        ax.clear()
        sequences = [int(item.get("sequence", 0)) for item in self.measurement_history]
        for light in LIGHTS:
            x: list[int] = []
            y: list[float] = []
            for sequence, item in zip(sequences, self.measurement_history):
                metric = item.get(light)
                if metric:
                    x.append(sequence)
                    y.append(float(metric["sum_net_1x"]))
            if y:
                ax.plot(x, y, marker="o", label=self._tr(
                    f"{self._light_label(light)} 增益归一化积分",
                    f"{self._light_label(light)} gain-normalized integral"))
        ax.set_title(self._tr(
            "重复测量稳定性（折算到 1× 增益）",
            "Repeated-measurement stability (normalized to 1× gain)"))
        ax.set_xlabel(self._tr("测量序号", "Measurement sequence"))
        ax.set_ylabel(self._tr("通道净信号总和 / 增益", "Sum of net channels / gain"))
        ax.grid(True, alpha=0.25)
        if ax.lines:
            ax.legend(loc="best")
        self.stability_figure.tight_layout()
        self.stability_canvas.draw_idle()

    def _redraw_temperature_plot(self) -> None:
        ax = self.temp_ax
        ax.clear()
        if self.temperature_history:
            t0 = self.temperature_history[0][0]
            x = [item[0] - t0 for item in self.temperature_history]
            y = [item[1] for item in self.temperature_history]
            ax.plot(x, y, marker=".")
        ax.set_title(self._tr("NTC 温度监测", "NTC temperature monitoring"))
        ax.set_xlabel(self._tr("相对时间 (s)", "Relative time (s)"))
        ax.set_ylabel(self._tr("温度 (°C)", "Temperature (°C)"))
        ax.grid(True, alpha=0.25)
        self.temp_figure.tight_layout()
        self.temp_canvas.draw_idle()

    def clear_measurements(self) -> None:
        self.ambient = [0] * len(self.channels)
        self.measurements.clear()
        self.measurement_history.clear()
        self.current_measurement_sequence = None
        self._update_spectrum_table()
        self._redraw_spectrum_plot()
        self._redraw_stability_plot()
        self.last_frame_var.set(self._tr("数据已清空", "Data cleared"))

    def export_current_csv(self) -> None:
        path = filedialog.asksaveasfilename(
            title=self._tr("导出当前光谱数据", "Export current spectral data"),
            defaultextension=".csv",
            filetypes=[(self._tr("CSV 文件", "CSV files"), "*.csv"),
                       (self._tr("所有文件", "All files"), "*.*")],
            initialfile=f"ams_spectral_{self.identity.family}_{datetime.now():%Y%m%d_%H%M%S}.csv",
        )
        if not path:
            return
        headers = ["channel", "ambient"]
        for light in LIGHTS:
            headers += [f"{light}_light", f"{light}_dark", f"{light}_net"]
        with open(path, "w", newline="", encoding="utf-8-sig") as handle:
            writer = csv.writer(handle)
            writer.writerow(["sensor_family", self.identity.family])
            writer.writerow(["sensor_candidates", self.identity.candidates])
            writer.writerow(["sensor_protocol", self.identity.protocol])
            writer.writerow(["requested_profile", self.identity.profile])
            writer.writerow(["effective_profile", self.identity.effective_profile])
            writer.writerow(["profile_ambiguous", self.identity.profile_ambiguous])
            writer.writerow(["i2c_address", self.identity.address])
            writer.writerow([])
            writer.writerow(headers)
            for idx, name in enumerate(self.channels):
                row: list[Any] = [name, self._safe_at(self.ambient, idx)]
                for light in LIGHTS:
                    frame = self.measurements.get(light)
                    if frame:
                        row += [self._safe_at(frame.light, idx),
                                self._safe_at(frame.dark, idx),
                                self._safe_at(frame.net, idx)]
                    else:
                        row += ["", "", ""]
                writer.writerow(row)
        self.log("SYS", self._tr(
            f"当前数据已导出：{path}", f"Current data exported: {path}"))

    def export_stability_csv(self) -> None:
        if not self.measurement_history:
            messagebox.showinfo(
                self._tr("稳定性数据", "Stability data"),
                self._tr("尚无完整四光源测量记录。",
                         "No complete four-LED measurement is available yet."))
            return
        path = filedialog.asksaveasfilename(
            title=self._tr("导出稳定性数据", "Export stability data"),
            defaultextension=".csv",
            filetypes=[(self._tr("CSV 文件", "CSV files"), "*.csv"),
                       (self._tr("所有文件", "All files"), "*.*")],
            initialfile=f"ams_spectral_stability_{datetime.now():%Y%m%d_%H%M%S}.csv",
        )
        if not path:
            return
        with open(path, "w", newline="", encoding="utf-8-sig") as handle:
            writer = csv.writer(handle)
            writer.writerow(["sensor_family", self.identity.family])
            writer.writerow(["effective_profile", self.identity.effective_profile])
            writer.writerow(["i2c_address", self.identity.address])
            writer.writerow([])
            writer.writerow([
                "sequence", "timestamp", "source", "gain_index", "flags",
                "temperature_c", "sum_net", "sum_net_at_1x", "peak_net",
            ])
            for item in self.measurement_history:
                for light in LIGHTS:
                    metric = item.get(light)
                    if metric:
                        writer.writerow([
                            item.get("sequence", 0),
                            datetime.fromtimestamp(float(item.get("time", 0))).isoformat(),
                            light, metric["gain"], metric["flags"],
                            metric["temperature"], metric["sum_net"],
                            metric["sum_net_1x"], metric["peak"],
                        ])
        self.log("SYS", self._tr(
            f"稳定性数据已导出：{path}", f"Stability data exported: {path}"))

    # ---------- 控制命令 ----------

    def toggle_stream(self) -> None:
        if self.stream_active:
            self.send_command("STOP")
            self.stream_active = False
            self.stream_button.configure(text=self._tr("开始连续读取", "Start streaming"))
            return
        try:
            interval = max(100, int(self.stream_interval_var.get()))
        except ValueError:
            messagebox.showwarning(
                self._tr("连续读取", "Streaming"),
                self._tr("周期必须是整数毫秒。", "The interval must be an integer in milliseconds."))
            return
        self.send_command(f"STREAM {interval}")

    def set_autogain(self) -> None:
        self.send_command(f"SET AUTOGAIN {1 if self.autogain_var.get() else 0}")

    def set_gain(self) -> None:
        try:
            index = GAIN_LABELS.index(self.gain_var.get())
        except ValueError:
            index = 5
        self.send_command(f"SET GAIN {index}")

    def set_atime(self) -> None:
        try:
            value = int(self.atime_var.get(), 0)
            if not 0 <= value <= 255:
                raise ValueError
        except ValueError:
            messagebox.showwarning(
                "ATIME", self._tr("ATIME 必须为 0～255。", "ATIME must be between 0 and 255."))
            return
        self.send_command(f"SET ATIME {value}")

    def set_astep(self) -> None:
        try:
            value = int(self.astep_var.get(), 0)
            if not 1 <= value <= 65534:
                raise ValueError
        except ValueError:
            messagebox.showwarning(
                "ASTEP", self._tr("ASTEP 必须为 1～65534。", "ASTEP must be between 1 and 65534."))
            return
        self.send_command(f"SET ASTEP {value}")

    @staticmethod
    def _number_text(text: str, maximum: int) -> int:
        value = int(text.strip(), 0)
        if not 0 <= value <= maximum:
            raise ValueError
        return value

    def read_register(self) -> None:
        try:
            bank = self._number_text(self.reg_bank_var.get(), 1)
            addr = self._number_text(self.reg_addr_var.get(), 255)
        except ValueError:
            messagebox.showwarning(
                self._tr("寄存器", "Register"),
                self._tr("Bank 或地址格式错误。可使用 0x 前缀。",
                         "The bank or address is invalid. A 0x prefix is accepted."))
            return
        self.send_command(f"AS REG READ {bank} {addr}")

    def write_register(self) -> None:
        try:
            bank = self._number_text(self.reg_bank_var.get(), 1)
            addr = self._number_text(self.reg_addr_var.get(), 255)
            value = self._number_text(self.reg_value_var.get(), 255)
        except ValueError:
            messagebox.showwarning(
                self._tr("寄存器", "Register"),
                self._tr("Bank、地址或写入值格式错误。",
                         "The bank, address or value is invalid."))
            return
        if not messagebox.askyesno(
                self._tr("寄存器写入", "Register write"),
                self._tr(
                    f"确认写入 Bank {bank} / 0x{addr:02X} = 0x{value:02X}？",
                    f"Write Bank {bank} / 0x{addr:02X} = 0x{value:02X}?")):
            return
        self.send_command(f"AS REG WRITE {bank} {addr} {value}")

    def dump_registers(self) -> None:
        try:
            bank = self._number_text(self.reg_bank_var.get(), 1)
            addr = self._number_text(self.reg_addr_var.get(), 255)
        except ValueError:
            messagebox.showwarning(
                self._tr("寄存器", "Register"),
                self._tr("Bank 或起始地址格式错误。",
                         "The bank or starting address is invalid."))
            return
        count = min(16, 256 - addr)
        self.send_command(f"AS DUMP {bank} {addr} {count}")

    def _toggle_sensor_monitor(self) -> None:
        if self.sensor_monitor_job:
            self.root.after_cancel(self.sensor_monitor_job)
            self.sensor_monitor_job = None
        if self.sensor_monitor_var.get():
            self._sensor_monitor_tick()

    def _sensor_monitor_tick(self) -> None:
        if not self.sensor_monitor_var.get():
            return
        if self.serial.connected:
            self.send_command("INFO", quiet=True)
        try:
            interval_ms = max(1000, int(float(self.sensor_monitor_interval_var.get()) * 1000))
        except ValueError:
            interval_ms = 5000
        self.sensor_monitor_job = self.root.after(interval_ms, self._sensor_monitor_tick)

    def _toggle_temperature_monitor(self) -> None:
        if self.temperature_monitor_job:
            self.root.after_cancel(self.temperature_monitor_job)
            self.temperature_monitor_job = None
        if self.temp_monitor_var.get():
            self._temperature_monitor_tick()

    def _temperature_monitor_tick(self) -> None:
        if not self.temp_monitor_var.get():
            return
        if self.serial.connected:
            self.send_command("TEMP", quiet=True)
        try:
            interval_ms = max(500, int(float(self.temp_interval_var.get()) * 1000))
        except ValueError:
            interval_ms = 2000
        self.temperature_monitor_job = self.root.after(interval_ms, self._temperature_monitor_tick)

    def on_close(self) -> None:
        self.sensor_monitor_var.set(False)
        self.temp_monitor_var.set(False)
        self.serial.disconnect()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    AmsSpectralApp(root)
    root.mainloop()


if __name__ == "__main__":
    main()
