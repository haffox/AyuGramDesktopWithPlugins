# Veyra Desktop

Independent AyuGram-based Telegram Desktop client with an embedded CPython plugin host.

- [Windows builds](https://github.com/haffox/AyuGramDesktopWithPlugins/actions/workflows/build.yml)
- [Installer and portable releases](https://github.com/haffox/AyuGramDesktopWithPlugins/releases)
- [Build instructions and plugin limitations](docs/PLUGINS.md)
- [Source attribution and licenses](NOTICE-VEYRA.md)

Windows output: `Veyra.exe`; installer: `Veyra-<version>-windows-x64-setup.exe`.
A successful build is only confirmed by a passing Actions run with downloadable artifacts.
Installers are unsigned. Plugins run trusted Python code without a sandbox; ExteraGram
compatibility is a documented subset, not Android/Xposed compatibility.

## Upstream and license

Based on [AyuGram Desktop](https://github.com/AyuGram/AyuGramDesktop) and
[Telegram Desktop](https://github.com/telegramdesktop/tdesktop).
Original author credits and copyright headers are preserved. See [LICENSE](LICENSE)
(GNU GPL v3) and [LEGAL](LEGAL). This fork is not an official upstream release.

The GitHub repository URL and internal SDK identifiers remain unchanged. Veyra uses
its own Windows application identity and installation directory; no automatic migration
or deletion of an existing AyuGram profile is performed. Back up `tdata` before any
manual profile transfer. The existing artwork has not been redesigned.
