@echo off
setlocal
rem 从脚本所在目录启动，避免双击时工作目录不一致。
cd /d "%~dp0"
python ams_spectral_sensor_app.py
if errorlevel 1 (
    echo.
    echo Application exited with an error. / 程序异常退出。
    pause
)

