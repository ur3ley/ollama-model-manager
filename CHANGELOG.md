# Changelog

All notable changes to this project are documented here.
This project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **Model hub**: *Installed* tab with repository cards and a hero strip (models, RAM, disk of
  Ollama's blob store, daemon health); *Ollama* tab (search ollama.com, pull with aggregate
  progress, update marks); *HuggingFace* tab (GGUF repositories, one entry per quant, split
  GGUFs and vision projectors, resumable downloads, import without keeping a second copy,
  HuggingFace token in KDE Wallet).
- Provenance (`models.json`): where each model came from, update checks, "Find sources".
- Themes (system / light / dark), editor with installable quants in the FROM picker,
  namespaced model names.
- Proxy: `[proxy] public_url` (endpoint shown to clients, DNS name in the certificate SAN),
  `[proxy] admin_api` with a per-token **Admin** flag, warning for `tls=off` on the LAN.
- 15 offline tests under CTest (`ctest --test-dir build`), including a fake `ollama` and a local
  HTTP server for the orchestration and download paths; `.desktop` file; CMake options
  `BUILD_TESTING`, `OMM_DEV_TOOLS`, `OMM_WARNINGS_AS_ERRORS`.

### Changed
- **`admin_api` defaults to `restricted`**: tokens without the Admin flag can no longer pull,
  create or delete models through the proxy (config without the key → restricted, logged at
  start). Set `admin_api = open` for the old behaviour.
- Downloaded GGUFs live in `~/.local/share/ollama-manager/models` (no spaces in `FROM`); the
  language and `models.json` moved under `~/.config/ollama-manager/`. Existing files are moved
  on the first start; see the README, "Where the files are".
- `ollama` operations are separate jobs: a second pull waits instead of killing the first,
  results can no longer be taken by another tab, the delete dialog no longer freezes the window.
- The proxy runs in its own thread; wallet prompts and certificate generation no longer freeze
  the event loop: the proxy keeps answering remote clients and the network keeps working while
  they run. Precisely: the window keeps repainting, but waits for the wallet prompt (up to
  5 minutes) and for the RSA key generation with user input excluded from the wait — clicks are
  ignored until it returns.
- Requires Qt 6.5 and CMake 3.21; the systemd unit installs to `share/ollama-model-manager/systemd`.

### Fixed
- HF downloads: write errors, size check, 416 on a complete `.part`, stall timeout, resume pinned
  to one commit; the token is not sent to CDN hosts on redirects.
- Modelfile: `"""` inside SYSTEM/TEMPLATE is refused; the system prompt keeps blank lines and
  indentation; repeated `stop` parameters are kept; GGUF files with nested arrays are read.
- Private temporary Modelfiles (0600, unique name, removed after `ollama create`); config
  directory 0700; `openssl` is taken from system directories first.

## [1.1.0] — 2026-11-21

### Added
- **Built-in auth proxy** (replacing the nginx + podman container): token checks run
  inside the manager, tokens are read from KDE Wallet and kept only as SHA-256 hashes
  in memory — no plaintext on disk, revocation is immediate. Token lookup supports
  `Authorization: Bearer` and `X-Api-Key`; streaming (`/api/chat` NDJSON, `/v1` SSE)
  is relayed unbuffered. See `docs/proxy.md`.
- Two listen modes: concrete interface addresses (may reuse Ollama's port) or
  `0.0.0.0` (must use another port, default `11435`).
- `proxy-selftest` — offline integration test with a fake upstream.
- **Daemon & tray** — `--daemon`, `--no-tray`, `--show`, single-instance via `QLocalServer`;
  closing the window hides it to the tray. Proxy settings live in
  `~/.config/ollama-manager/config.ini` (`[proxy]`), with a live bind check.
- **TLS on the internal hop** — internal Root CA (10 years) + 90‑day leaf with automatic
  renewal, or user‑provided cert/key. `QSslServer`, `--tls off|internal|files`.
- **Runtime localization** — plain `.ini` key‑value files in `languages/` folder.
  `en.ini` (English, base & fallback) and `ru.ini` (Russian) included.
  Adding a language requires no recompilation: copy a file, translate it,
  restart — the new language appears in the **Language** menu automatically.
  Language choice is remembered (since the next release in `config.ini [ui]`).
- `_HOWTO.txt` in `languages/` with instructions for contributors.
- **API token manager** (`Tools → API tokens…`, `Ctrl+T`): one token per device
  (laptop, phone, …), issued/revoked from the GUI and stored **only in KDE Wallet**
  (wallet `kdewallet`, folder `ollama-auth`). The built-in proxy checks them as
  SHA-256 hashes in memory — no sync script, no tmpfile, revocation is immediate.
  Requires `KF6::Wallet`; `tests/token-selftest.cpp` is a manual smoke test.

### Changed
- The former `ollama-auth` project has been merged into this repository; the old nginx
  container pipeline has been removed from the repository and
  is superseded by the built-in proxy.
- All UI strings moved from hard‑coded `tr(...)` to `T("key")` → `languages/*.ini`.
  Main window, model list, model editor, GGUF import dialog and rename dialog
  all support live retranslation via `retranslateUi()`.

### Fixed
- **TLS connection-budget leak** — the pre-handshake socket budget was released only via a
  `destroyed` connection that was torn down after a successful handshake, so after `maxConnections`
  successful requests the proxy refused everyone. Budget release is now idempotent and covered by
  regression tests.
- **Multimodality (vision) loss when editing a model.** Rebuilding a model now
  references it by name in `FROM` instead of the raw weights blob path, so the
  vision projector, template, renderer and parser layers are inherited instead
  of being silently dropped.
- **Cross-contaminated `num_ctx` display** for models sharing the same weights
  blob. The editor now reads the opened model itself rather than an arbitrary
  sibling detected via the shared blob.

### Changed
- **Context size input is now in K units** (1K = 1024 tokens). Integer spinbox
  with a companion slider (70/30 split) and a static `K` label.
- Context input is hard-capped at the model's maximum context; over-limit values
  loaded from a model are clamped and highlighted in red.

### Security
- Hardened the built-in proxy after an external audit (0 critical; all high/medium findings fixed):
  body accounting by actual bytes with bidirectional backpressure, a socket budget that covers
  pre-handshake connections, a header-read timeout, strict `Content-Length`/duplicate/`TE`+`CL`
  validation, hop-by-hop header stripping (including headers named in `Connection`), device-name
  validation, rejection of control characters/obs-fold, upstream read/write timeouts, atomic TLS
  writes with restart rollback, symlink policy for key files, log sanitisation, and a hardened
  systemd unit (`NoNewPrivileges`, `ProtectSystem`, `RestrictAddressFamilies`, `LimitCORE=0`).
- Parser fuzzing: >4.5 million libFuzzer iterations under ASan/UBSan, zero crashes.

## [1.0.0]

### Added
- Model browser (name, ID, size, modified date).
- Parameter editor: `num_ctx`, `temperature`, `top_p`, `top_k`,
  `repeat_penalty`, `repeat_last_n`, `presence_penalty`, `frequency_penalty`,
  and a custom `SYSTEM` prompt.
- Create, update, rename (`ollama cp`) and delete models.
- `Modelfile` preview before applying.
- GGUF import.
