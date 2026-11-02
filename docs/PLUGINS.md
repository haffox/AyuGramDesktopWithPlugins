# AyuGram Desktop: embedded Python integration (Windows x64)

This is the **actual AyuGram source tree**, based on
`AyuGram/AyuGramDesktop@0f2a31bb420de98f331990432265b131b8d836b5`.
It is not the archive's standalone extension test project and does not build a
second, unmodified baseline. The Telegram CMake target contains the plugin host.

## Import into your GitHub repository

Preferred: clone the supplied Git bundle (contains a complete source snapshot and
all 36 upstream gitlinks, not the full upstream commit history):

```sh
git clone --branch main AyuGramDesktop-plugins.bundle AyuGramDesktop
cd AyuGramDesktop
git remote set-url origin https://github.com/YOUR_ACCOUNT/YOUR_REPOSITORY.git
git push -u origin main
```

If using the source ZIP instead, extract it and run:

```sh
python scripts/bootstrap_repository.py
git remote add origin https://github.com/YOUR_ACCOUNT/YOUR_REPOSITORY.git
git push -u origin main
```

Do **not** just `git init && git add .` after unzipping: Git ZIP archives do not
preserve submodule index entries. The bootstrap script restores the locked entries.
No original upstream files are downloaded over your edited source during CI.

## CI build and installer

Every push, on any branch, starts `.github/workflows/build.yml`:

1. Check out this source and recursively initialize pinned upstream submodules.
2. Keep a CPython **3.12.10 x64 SDK** separate from Python 3.10 build tools.
3. Initialize VS 2022 **v143 / 14.44**, Windows SDK **10.0.26100.0**.
4. Run the upstream `prepare/win.bat qt6 silent` for the pinned tree's static Qt6
   and other native dependencies; cache `Libraries/` and `ThirdParty/`.
5. Configure and build the **modified** `Telegram` target, Release x64.
6. Bundle the hash-verified CPython embeddable distribution, DLLs and compatibility SDK.
7. Build an **unsigned** Inno Setup installer and a portable ZIP; upload both
   plus SHA256SUMS to `Veyra-windows-x64` in Actions artifacts.

Expected outputs:

- `Veyra-7.2.9-windows-x64-setup.exe`
- `Veyra-windows-x64-portable.zip`
- `SHA256SUMS.txt`

The installer has a distinct application ID and does not delete existing profiles.
Stock automatic updates are disabled so they cannot overwrite the plugin-enabled
executable with an upstream build. Signing requires your own certificate and is
not configured or claimed here.

The workflow's API ID/hash defaults are the example values in upstream
`docs/building-win.md`. Before distribution, set repository variables
`TDESKTOP_API_ID` and `TDESKTOP_API_HASH` to your own Telegram application's values.

Cold TDesktop dependency builds can exhaust the hosted runner's disk or six-hour
job limit; a YAML file cannot guarantee otherwise. For repeated builds use a warm
cache or set `WINDOWS_BUILD_RUNNER` to the label of a dedicated Windows runner
with VS v143/14.44, SDK 26100, Git, Chocolatey, and sufficient free disk (plan for
at least 100 GB). The default runner is `windows-2025`; no successful hosted run
is asserted by this source snapshot.

## Local Windows build

Clone and initialize submodules, in a directory without spaces, such as
`D:\AyuBuild\tdesktop`:

```bat
git submodule update --init --recursive
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" -vcvars_ver=14.44
set AYU_PYTHON_SDK=C:\Path\To\Python312
set TDESKTOP_API_ID=YOUR_ID
set TDESKTOP_API_HASH=YOUR_HASH
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\windows\build.ps1
```

Use Python 3.10 with CMake 3.31.6 and Ninja 1.11.1.4 on PATH for the preparation
scripts. `AYU_PYTHON_SDK` is a **full** CPython 3.12.10 installation (headers and
`libs/python312.lib`), not the embeddable ZIP. Inno Setup 6 must be installed or
Chocolatey must be able to install it. Build scripts create sibling `Libraries`,
`ThirdParty`, `PythonRuntime`, `package`, `build-logs`, and `artifacts` directories.

This integration's packaging and adapter target Windows x64/Qt6/Release.
Linux, macOS, ARM, Qt5, and Debug CPython deployment are not configured by this patch.

## Using plugins

- Open **Settings → Плагины**. Plugins are disabled until explicitly enabled.
- Put trusted `.py` plugin files in
  `QStandardPaths::AppDataLocation/ayu-plugins/plugins` (normally below
  `%APPDATA%/AyuGramDesktop`); actual Qt organization settings determine the prefix.
- Per-account grants are stored in `ayu-plugins/account-<session.uniqueId>.ini`.
- Copy the supplied `plugin-examples/*.py` there if desired; examples are not
  auto-enabled or silently granted permissions.
- Use **Перезагрузить плагины** after adding or editing files.
- Local commands use `/plugin: plugin_id/command arguments`; this namespace is
  consumed locally even if the command is unavailable.
- Message-context menu descriptors are rendered in the main HistoryInner chat menu
  and HistoryView::ListWidget menus. Old account/generation callbacks are rejected.

## Exact integration points

- `apiwrap.cpp / ApiWrap::sendMessage`: intercept a text intent **before** ghost
  scheduling, optimistic insertion, splitting or RPC. Keep reply/thread/options,
  target account and recipient; resume exactly once with an intent-local bypass.
- `apiwrap.cpp / ApiWrap::sendRichMessage(page, ...)`: intercept the flattened text
  before local insertion. An unchanged result preserves the rich message; changing
  text converts that intent to ordinary plain text because this SDK has no rich-block
  editing model. The low-level item/RPC overload is not intercepted a second time.
- `data/data_session.cpp / Session::addNewMessage`: handle live `Unread` additions
  and adopted streamed messages **after** native reconciliation and model insertion.
  Outgoing echoes, history pagination, service messages and background accounts
  are excluded. Already-intercepted items are not intercepted twice.
- `HistoryItem`: transient presentation override, epoch and globally unique request
  token. Edits invalidate the token; deletion is checked by resolving FullMsgId again.
  Canonical server text, MTProto pts/seq, storage and export are not rewritten.
- `history_view_element.cpp`: defer visibility while the incoming hook is pending,
  then render the returned text without reusing obsolete entity offsets.
- `ayu/utils/telegram_helpers.cpp`: final DROP uses AyuGram's existing shared local
  hidden-message policy. Pending hook work does not itself hide global notification
  queues. Chat previews, copy/export/search text remain canonical; this is **not**
  a promise to redact all surfaces or to retract a notification already shown.
- `core/application.cpp`: one interpreter per process; account-switch/passcode-lock
  epoch revocation; explicit worker shutdown before the domain and windows vanish.
- `settings/sections/settings_main.cpp`: reachable settings dialog, grants, reload,
  error console and QSS file watcher. QSS affects Qt widgets, not all custom painting.

The SDK's send API uses **opaque encoded PeerId strings in the current account**,
not Android numeric dialog IDs. It resolves already-known peers only, uses the
native sending pipeline and declines paid messages and active slow mode. Plugin
origin is tagged per intent, so plugin-generated text cannot recursively trigger
outgoing hooks. A successful return means accepted for sending, not delivered.

Cancellation consumes the outgoing intent; this patch does not restore a compose
field already cleared by the UI. Media/file sends, media-caption editing, edits to
already-sent messages, raw TL requests and all-account background interception
are outside the text SDK's contract.

## exteraGram compatibility is a subset, not Android emulation

The archive's Python facade is preserved: `BasePlugin`, the send hook with
DEFAULT/CANCEL/MODIFY, selected UI/menu descriptors and text-only client utilities.
It does **not** make arbitrary Android plugins binary/API-compatible with Desktop.
Java/Xposed, Android UI classes, raw TL hooks, MODIFY_FINAL, persistent Android
settings editors, arbitrary menu placements and pip dependency auto-install remain
explicitly unsupported. Unsupported APIs raise exceptions rather than silently
pretending to work. See `Telegram/Resources/plugin-sdk/` for the exact contract.

Only install trusted code. CPython is embedded in the client process, not sandboxed;
SDK permissions cannot prevent filesystem/network access or interrupt a native
extension that hangs. A hung plugin can block interpreter shutdown. Strong isolation
would require a separate plugin process, which this archive's engine does not supply.

## Verification boundary

See `docs/PLUGIN_INTEGRATION_VERIFICATION.md`. Source integration and static checks
are not a Windows build log. Do not describe the installer as successfully built
until the actual Windows job completes and its artifact is downloaded and launched.
