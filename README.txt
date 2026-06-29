Broadcast Graphics Live - Ubuntu 26.04 WSL2 build tools

Run from PowerShell:
  powershell -NoProfile -ExecutionPolicy Bypass -File .\build-ubuntu-wsl.ps1

Clean build:
  powershell -NoProfile -ExecutionPolicy Bypass -File .\build-ubuntu-wsl.ps1 -Clean

Revision 2026-06-28.2 fixes WSL2 detection by reading `wsl.exe --list --verbose`.
It no longer starts the distro before a required WSL1-to-WSL2 conversion and skips
`--set-version` entirely when Ubuntu-26.04 is already WSL2.
