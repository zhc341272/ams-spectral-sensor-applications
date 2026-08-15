#!/usr/bin/env sh
set -eu
# 固定工作目录，保证从文件管理器或终端启动时行为一致。
cd "$(dirname "$0")"
exec python3 ams_spectral_sensor_app.py

