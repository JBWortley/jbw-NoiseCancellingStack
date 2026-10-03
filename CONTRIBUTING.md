# Contributing

Contributions are welcome, especially compatibility reports, rollback tests, and narrowly scoped safety improvements.

## Before opening a change

- Read [docs/SAFETY.md](docs/SAFETY.md). Its invariants are release requirements, not suggestions.
- Do not commit recordings, registry exports, endpoint GUIDs, machine names, logs, downloaded binaries, signing keys, or credentials.
- Keep render/playback endpoints out of scope. This project manages capture endpoints only.
- Do not weaken the 48 kHz RNNoise guard, existing-APO preservation, conflict-aware rollback, or fixed-path SYSTEM-task checks.
- Open a private security report instead of a public issue when a bug could enable privilege escalation, arbitrary registry writes, path traversal, unsafe DLL loading, or destructive rollback.

## Development workflow

Use Windows 10 or 11 x64 with the SDK selected by `global.json`:

```powershell
dotnet test .\NoiseCancellingStack.slnx --configuration Release
powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1
```

The full build also requires Inno Setup 6 and downloads only checksum-pinned upstream archives. Never update an upstream URL, version, or hash without documenting where it came from and reviewing the new source and license.

Changes that touch endpoint registration or uninstall should include tests and a written rollback scenario. Changes to the installer should be tested through a real install, repair, uninstall, and reinstall on a disposable or recoverable Windows system.

## License of contributions

By submitting a contribution, you agree that your project-authored contribution may be distributed under the repository's MIT License. Do not submit code you are not authorized to license on those terms.

Third-party code must not be copied into the repository casually. Identify its origin and license first, then update [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the release-source process as required.
