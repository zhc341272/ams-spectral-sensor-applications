@echo off
setlocal
rem 使用当前 Python 安装上位机依赖，建议先创建虚拟环境。
python -m pip install --upgrade pip
if errorlevel 1 goto :failed
python -m pip install -r "%~dp0requirements.txt"
if errorlevel 1 goto :failed
echo.
echo Dependencies installed successfully. / 依赖安装完成。
pause
exit /b 0

:failed
echo.
echo Installation failed. Check Python and network settings. / 安装失败，请检查 Python 和网络。
pause
exit /b 1

