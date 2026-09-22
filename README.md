# mnpdf

A lightweight native Windows PDF reader with text selection, search, highlights,
reversible saving, sticky-note pins, undo/redo, and a small private-memory
footprint. It runs locally with Win32 and PDFium - no accounts or network
services.

## Release

Download the Windows portable package from the [Releases](https://github.com/mnsky-tyan/mnpdf/releases) page. Extract `mnpdf-win-x64.zip` and run `mnpdf.exe`; keep `pdfium.dll` beside it.

## Build

Requirements: Windows, Visual Studio 2022 Enterprise C++ tools, and the vendored
PDFium files.

```bat
build.bat
```

The executable and PDFium runtime are written to `build/`. The checked-in
PowerShell suites exercise the public UI through posted Windows messages:

```powershell
powershell -ExecutionPolicy Bypass -File select-msg-test.ps1
powershell -ExecutionPolicy Bypass -File test-suite2.ps1
powershell -ExecutionPolicy Bypass -File test-features.ps1
powershell -ExecutionPolicy Bypass -File test-continuous.ps1
```

## Use

Run `mnpdf.exe path\to\file.pdf`. Right-click the page for document actions;
select text and right-click for highlighting; right-click paper to add a pin.
Highlights and pins are kept reversible through PDF annotations and the local
sidecar state. Pin and highlight colors share six built-ins plus three persisted
custom `#RRGGBB` slots.
