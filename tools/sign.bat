@echo off

setlocal

set SYS_PATH=%~1
if "%SYS_PATH%"=="" set SYS_PATH=%~dp0shared_mapping_driver.sys
set CERT_NAME=SharedMappingTest

if not exist "%SYS_PATH%" (
    echo [!] %SYS_PATH% not found. Build the project first.
    exit /b 1
)

set "KITS=%ProgramFiles(x86)%\Windows Kits\10\bin"
set SIGNTOOL=
for /f "delims=" %%v in ('dir /b /ad "%KITS%" ^| findstr /r "^10\."') do (
    if exist "%KITS%\%%v\x64\signtool.exe" set "SIGNTOOL=%KITS%\%%v\x64\signtool.exe"
)
if not defined SIGNTOOL (
    echo [!] signtool.exe not found under Windows Kits. Install the WDK.
    exit /b 1
)

powershell -NoProfile -Command "if (-not (Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=%CERT_NAME%' })) { exit 1 }" >nul 2>&1
if %errorlevel% neq 0 (
    echo [*] Creating test certificate...
    powershell -NoProfile -Command "$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=%CERT_NAME%' -FriendlyName '%CERT_NAME%' -CertStoreLocation Cert:\CurrentUser\My; Export-Certificate -Cert $cert -FilePath '%~dp0%CERT_NAME%.cer'" >nul 2>&1
    if not exist "%~dp0%CERT_NAME%.cer" (
        echo [!] Failed to create certificate.
        exit /b 1
    )
    echo [+] Certificate created. On the test VM run once: tools\cert-add.bat
)

echo [*] Signing %SYS_PATH% ...
"%SIGNTOOL%" sign /s My /n %CERT_NAME% /fd SHA256 "%SYS_PATH%"
if %errorlevel% neq 0 (
    echo [!] Signing failed.
    exit /b 1
)

echo [+] Driver signed.
