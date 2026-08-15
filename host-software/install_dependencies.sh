#!/usr/bin/env sh
set -eu
# 使用当前 Python 解释器安装依赖；Linux 还需要系统提供 Tk。
python3 -m pip install --upgrade pip
python3 -m pip install -r "$(dirname "$0")/requirements.txt"

