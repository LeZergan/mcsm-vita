@echo off
powershell -ExecutionPolicy Bypass -File "%~dp0get_dump.ps1" -VitaIp %1 -OutputPath %2
