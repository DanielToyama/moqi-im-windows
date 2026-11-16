@echo off
chcp 65001 >nul
set "MOQI_INSTALL_PS1=%~dp0install-release.ps1"
if not exist "%MOQI_INSTALL_PS1%" set "MOQI_INSTALL_PS1=%~dp0install.ps1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%MOQI_INSTALL_PS1%" -NoPause
set "MOQI_INSTALL_EXIT=%ERRORLEVEL%"
echo.
if not "%MOQI_INSTALL_EXIT%"=="0" (
  echo 安装失败，错误码 %MOQI_INSTALL_EXIT%。请查看上方提示。
) else (
  echo 安装完成。请重新打开正在使用输入法的应用。
)
pause
