@echo off
setlocal

set DRIVER_NAME=smmap
set SYS_PATH=%~dp0shared_mapping_driver.sys
set DEST_PATH=%SystemRoot%\System32\drivers\%DRIVER_NAME%.sys

if not exist "%SYS_PATH%" (
    echo [!] %SYS_PATH% not found.
    exit /b 1
)

sc query %DRIVER_NAME% >nul 2>&1
if %errorlevel% equ 0 (
    echo [*] Stopping %DRIVER_NAME%...
    sc stop %DRIVER_NAME% >nul 2>&1
    timeout /t 1 /nobreak >nul
    echo [*] Deleting old service...
    sc delete %DRIVER_NAME% >nul 2>&1
    timeout /t 1 /nobreak >nul
)

echo [*] Copying driver to %DEST_PATH%...
copy /y "%SYS_PATH%" "%DEST_PATH%" >nul
if %errorlevel% neq 0 (
    echo [!] Failed to copy driver. Run as Administrator.
    exit /b 1
)

echo [*] Creating service...
sc create %DRIVER_NAME% type=kernel binPath="%DEST_PATH%"
if %errorlevel% neq 0 (
    echo [!] Failed to create service.
    exit /b 1
)

echo [*] Starting driver...
sc start %DRIVER_NAME%
if %errorlevel% neq 0 (
    echo [!] Failed to start driver. Check: test signing on, HVCI off,
    echo     cert-add.bat run once on this VM, WinDbg for details.
    exit /b 1
)

echo [+] %DRIVER_NAME% loaded successfully.
