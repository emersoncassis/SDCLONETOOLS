#!/bin/sh
# Compila o SDCLONETOOLS para Windows a partir de Linux/macOS/Windows (precisa de: pip install ziglang)
set -e
L="-lcomctl32 -lcomdlg32 -lshell32 -lole32 -ladvapi32 -lgdi32 -luser32 -lsetupapi"
F="-O2 -s -Wno-format -Wno-unused-function -Wl,--subsystem,windows"
python3 mkicon.py
python3 -m ziglang cc -target x86_64-windows-gnu $F -o SDCloneTools.exe   sdclone.c sdclone.rc $L
python3 -m ziglang cc -target x86-windows-gnu    $F -o SDCloneTools32.exe sdclone.c sdclone.rc $L
