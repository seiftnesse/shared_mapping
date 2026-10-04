@echo off

setlocal

if not exist "%~dp0SharedMappingTest.cer" (
    echo [!] %~dp0SharedMappingTest.cer not found. Run sign.bat first.
    exit /b 1
)

certutil -addstore TrustedPublisher "%~dp0SharedMappingTest.cer"
certutil -addstore Root "%~dp0SharedMappingTest.cer"
