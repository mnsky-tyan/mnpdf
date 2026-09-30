# mnpdf-native - project knowledge

Windows-only native PDF reader: `src/main.cpp` (the whole app), PDFium in
`third_party/`, PowerShell suites that drive the real binary. Extension-less
learning about building and testing this repo; the reader-facing description
of the app lives in `README.txt` (shipped) and `README.md` (repository page).

## Test and pipeline layout

- Two entry points, one set of suites. `scripts/gate-test.sh` (WSL: adds a
  drive-letter guard, a checkout lock, isolated per-suite state) and
  `scripts/test-ci.ps1` (plain Windows: the same build and the same six
  suites, no `wslpath`). The GitHub workflow runs the latter on a Windows
  runner, so a pull request always has a real check.
- `origin` is `https://github.com/mnsky-tyan/mnpdf.git`, because `no-mistakes`
  opens PRs against `origin`. `localmirror` is the old local bare repo, kept
  for a byte-for-byte backup push. The `no-mistakes` remote is the gate's own
  repo - never repoint it.
- Suites run one at a time, each with its own `APPDATA`/`TEMP`. A leftover
  instance of the built binary makes every suite pre-flight skip, and a build
  cannot replace `build/mnpdf.exe` while one holds it: kill leftovers first,
  and always after a run.
- `no_ci` is gone from `.no-mistakes.yaml`; CI exists now.

## Facts that cost real time to learn

- Dismissing a dialog: send the command to the dialog itself
  (`SendMessageW(dlg, WM_COMMAND, id, 0)`). Posting `WM_COMMAND` to the dialog
  does nothing (measured: a confirmation stayed up and the app thread stayed
  modal), and `BM_CLICK` on the button is dropped while the dialog is still
  being built. Click the button only as a fallback.
- A posted `WM_ACTIVATE` is ignored by Windows - activation messages are not a
  program's to post. `WM_KILLFOCUS` is the one that reliably reaches a
  subclassed control's focus-loss path.
- The app checks only that an environment variable exists, never its value:
  `MNPDF_BACKGROUND=0` still starts the window minimised. A suite that has to
  type into the app needs the variable *removed* (`unset` in bash,
  `Remove-Item Env:...` in PowerShell), not set to 0. The gate and
  `test-ci.ps1` both do this for `test-colors.ps1` only.
- The custom colour box rejects a focus loss in its first 400 ms on purpose,
  so a probe has to wait out a reader's pause before it clicks away. Non-hex
  keystrokes never reach the box at all - the honest bad entry is hex digits
  in the wrong count.
- `PrintWindow` must be handed a bitmap the size of the whole client. A bitmap
  shorter than the client is clipped and the measured band comes out empty.
  A minimised client rect is 0x0, so hidden windows cannot be pixel-checked -
  which is why the colour suite is the one foreground suite.
- `RECT` field order is `left, top, right, bottom`; a wrong order silently
  turns every client measurement negative.
- `Start-Process -Wait -PassThru -RedirectStandardOutput` deadlocks when the
  child launches a GUI app: .NET also waits for the redirected pipe to close,
  and the app inherits the handle. Let the child write its own log
  (`Tee-Object -FilePath ...`) and call the child directly.
- `cmd /c build.bat` fails ("not recognized"): address the `.bat` absolutely,
  since `cmd` resolves a bare script name against the PATH.
- `$PSScriptRoot` inside `scripts/test-ci.ps1` is `scripts\`, not the repo
  root - the root is one level up.
- An `Edit` control's text must be read with `SendMessageW(WM_GETTEXT)` into a
  `[Out] char[]`. `GetWindowTextW` answers with the creation text across
  processes.
