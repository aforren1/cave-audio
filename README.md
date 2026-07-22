# bw_audio - Godot addon (distribution branch)

This branch is machine-published by CI on every release tag. It exists so the Godot Asset
Library / Asset Store, which download a repository archive at a pinned commit, can serve the
addon WITH its binaries - which are deliberately not committed to `main`.

The addon is `addons/bw_audio/`: a GDExtension control client for the bw_audio spatial
audio engine (26-speaker CAVE array over ASIO, binaural monitor). Windows x64 only.

- Install: copy `addons/bw_audio/` into your project, restart the editor. Nothing to enable.
- Try it: open `addons/bw_audio/playground/playground.tscn` and press play.
- Docs, license, and the exact source commit: inside `addons/bw_audio/`.
- Source, issues, releases: https://github.com/aforren1/cave-audio (branch `main`).

If this file ended up in your project root via an Asset Library install, it is safe to delete
- only `addons/bw_audio/` matters.
