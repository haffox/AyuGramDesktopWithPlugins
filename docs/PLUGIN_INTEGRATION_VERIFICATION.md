# Verification of this integration snapshot

## Performed

- Integrated the supplied engine files directly under `Telegram/SourceFiles/plugins`.
- Read and patched the actual pinned upstream message, settings and lifecycle code;
  no guessed `onMessageReceived` or nonexistent host API calls were added.
- `git diff --check`: passed.
- GitHub Actions workflow: actionlint 1.7.7, passed.
- PowerShell syntax: tree-sitter-powershell parsed `scripts/windows/build.ps1` and
  every workflow PowerShell run block without error/missing nodes.
- Python syntax: AST parsed all 10 SDK/example/bootstrap `.py` files without errors.
- All 36 top-level submodule paths matched `.gitmodules`; exact gitlink hashes are
  recorded in `plugins-upstream.lock.json` and retained in the Git snapshot.
- Downloaded official CPython 3.12.10 x64 embeddable archive and calculated SHA256:
  `4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3`.
  The Windows build script verifies this hash before extracting the runtime.

- Extracted the source ZIP and ran its repository bootstrap: all 36 gitlinks
  and the complete Git tree matched the exported snapshot on Linux.
- `git bundle verify` accepted the self-contained source Git bundle.

## Not performed / not claimed

This Linux environment did not provide MSVC, the Windows SDK or the prepared
AyuGram/Qt native dependency tree. Therefore:

- No full native Windows CMake configure/build was executed.
- No passing GitHub Actions run was produced.
- No `.exe` was compiled, installed or launched here.
- Static syntax validation is **not** C++ type checking or link validation.
- Installer behavior, real account sends, incoming display timing, plugin menus,
  lock/account transitions and runtime shutdown still need a Windows integration run.

The workflow builds the modified client itself and packages its output; it does
not substitute standalone smoke tests or a separate unmodified baseline. A build
failure uploads configure/prepare/compile/installer logs rather than claiming success.

Expected next verification: push this Git snapshot to the destination repository,
let the Windows job finish, download its unsigned setup/portable artifacts and
verify installation and an explicitly enabled trusted plugin with a test account.
