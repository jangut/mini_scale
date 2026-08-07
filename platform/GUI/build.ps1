# One-click build script for the scale GUI (Windows + PyInstaller).
# Usage:  .\build.ps1    (or just double-click build.bat)
# Output: dist\MiniScale\MiniScale.exe

Set-Location $PSScriptRoot

Write-Host "== 1/4  Check / install PyInstaller =="
python -m pip install pyinstaller --quiet
if ($LASTEXITCODE -ne 0) { throw "pip install pyinstaller failed" }

Write-Host "== 2/4  Generate logo.ico from LOGO.png (via Qt, no Pillow) =="
python -c "from PySide6.QtGui import QImage; img = QImage('LOGO.png'); img.scaled(256, 256).save('logo.ico')"
if ($LASTEXITCODE -ne 0) { throw "icon conversion failed" }

Write-Host "== 3/4  Clean previous build =="
Remove-Item -Recurse -Force dist, build -ErrorAction SilentlyContinue

Write-Host "== 4/4  PyInstaller (single-file) =="
# 排除 speech 环境里与上位机无关的第三方大包，避免 exe 体积膨胀
# （torch/sklearn/librosa 等均未被本项目 import，PyInstaller 却会误收集）
python -m PyInstaller --noconfirm --clean --windowed --onefile --name MiniScale `
    --icon logo.ico `
    --add-data "LOGO.png;." `
    --exclude-module torch --exclude-module torchvision --exclude-module torchaudio `
    --exclude-module sklearn --exclude-module scipy --exclude-module matplotlib `
    --exclude-module pandas --exclude-module numba --exclude-module llvmlite `
    --exclude-module librosa --exclude-module sympy --exclude-module PIL `
    --exclude-module transformers --exclude-module tensorflow --exclude-module onnxruntime `
    --exclude-module PySide6.QtQml --exclude-module PySide6.QtQuick `
    --exclude-module PySide6.QtQuickWidgets --exclude-module PySide6.QtMultimedia `
    --exclude-module PySide6.QtMultimediaWidgets --exclude-module PySide6.QtCharts `
    --exclude-module PySide6.QtDataVisualization --exclude-module PySide6.Qt3DCore `
    --exclude-module PySide6.Qt3DRender --exclude-module PySide6.QtPdf `
    --exclude-module PySide6.QtPdfWidgets --exclude-module PySide6.QtPositioning `
    --exclude-module PySide6.QtSensors --exclude-module PySide6.QtBluetooth `
    --exclude-module PySide6.QtWebSockets --exclude-module PySide6.QtWebChannel `
    --exclude-module PySide6.QtDesigner --exclude-module PySide6.QtHelp `
    --exclude-module PySide6.QtUiTools --exclude-module PySide6.QtSql `
    --exclude-module PySide6.QtTest `
    main.py
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }

$exe = Join-Path $PSScriptRoot "dist\MiniScale.exe"
Write-Host ""
Write-Host "Build OK: $exe"
Write-Host "A single portable exe. Copy MiniScale.exe anywhere; no Python needed."
Write-Host "Note: first launch unpacks to temp, so it may take a few seconds to start."
