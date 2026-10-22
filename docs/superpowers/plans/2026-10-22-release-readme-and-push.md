# Release README and Push Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish the completed 1.1.0 dual-rate noise-filtering implementation with a clear product README and a freshly verified installer release set.

**Architecture:** Keep the product behavior unchanged. Improve the repository entry point so readers can immediately understand rate-based routing, application coverage, startup repair, limitations, installation, and verification; then rebuild because `README.md` is embedded in the installer and project source archive.

**Tech Stack:** Markdown, PowerShell release scripts, .NET 9, Meson/C++, Inno Setup, Git.

**Spec:** `docs/superpowers/specs/2026-10-22-webrtc-apm-16khz-fallback-design.md`

## Global Constraints

- Work directly on `main` as requested.
- Preserve 48 kHz RNNoise routing, 16 kHz WebRTC APM routing, and unchanged pass-through at every other sample rate.
- Describe system-wide coverage accurately: Windows shared-mode applications using system effects are covered; exclusive, ASIO, enhancement-disabled, and other APO-bypassing paths are not.
- Do not claim that noise suppression repairs codec bandwidth, clipping, distance, reverberation, or radio-link problems.
- Keep release version `1.1.0` and preserve all corresponding-source and license artifacts.
- Use background CLI operations only; do not activate visible applications or access peripherals.

## Review Focus

- A reader must be able to determine which processor handles a microphone from its active sample rate.
- WD Studio and similar applications must not be described as covered when they bypass Windows system effects.
- Startup repair must be described as endpoint discovery/reattachment, not as unconditional Windows Audio restarts.
- The unsigned-installer and checksum-verification guidance must remain prominent.
- The rebuilt installer and source archive must contain the updated README without omitting release files.

---

### Task 1: Refresh and publish the 1.1.0 release documentation

**Files:**
- Modify: `README.md`
- Create: `docs/superpowers/plans/2026-10-22-release-readme-and-push.md`
- Regenerate: `dist/NoiseCancellingStack-Setup-1.1.0.exe`
- Regenerate: `dist/NoiseCancellingStack-source-1.1.0.zip`
- Regenerate: `dist/SHA256SUMS.txt`

**Interfaces:**
- Consumes: The routing, startup-repair, safety, compatibility, build, and packaging behavior implemented on local `main`.
- Produces: A public README and complete release set suitable for `origin/main` at `https://github.com/JBWortley/jbw-NoiseCancellingStack`.

- [ ] **Step 1: Rewrite the README's opening and usage guidance**

Add a compact routing table, make Windows shared-mode application coverage and automatic recovery easy to scan, retain compatibility boundaries, and improve installation/release guidance without changing technical claims.

- [ ] **Step 2: Review the rendered Markdown and repository diff**

Run: `git diff --check` and inspect `git diff -- README.md`.

Expected: no whitespace errors; wording agrees with the approved design and implementation.

- [ ] **Step 3: Rebuild the complete release set**

Run: `powershell -ExecutionPolicy Bypass -File .\scripts\build.ps1`

Expected: exit code `0`; managed tests, native tests, publication, archive creation, and Inno Setup packaging complete successfully.

- [ ] **Step 4: Verify the generated release artifacts**

Recompute every digest listed in `dist/SHA256SUMS.txt`, inspect installer version metadata, confirm the updated README is present in the project source archive, and run `git diff --check` plus `git status --short`.

Expected: all manifest digests match, installer product version is `1.1.0`, archived README matches the worktree README, and only intended tracked files are pending.

- [ ] **Step 5: Commit and push**

Run: `git add README.md docs/superpowers/plans/2026-10-22-release-readme-and-push.md`, commit with `docs: refresh 1.1.0 release guide`, then push `main` to `origin`.

Expected: `git push origin main` succeeds and local `main` matches `origin/main`.
