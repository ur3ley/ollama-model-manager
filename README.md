# Ollama Model Manager

A lightweight desktop GUI for managing your local [Ollama](https://ollama.com) models — built with Qt 6 and C++17.

Browse installed models, tweak their parameters (context size, sampling, penalties, system prompt), create new variants, import GGUF files, rename and delete — all without hand‑writing a single `Modelfile`.

> **Highlight:** editing a model **preserves its multimodal (vision) projector**, template, renderer and parser. Unlike a naïve `ollama create FROM /path/to/blob`, this tool rebuilds models by referencing them **by name**, so capabilities like `vision` are never silently dropped.

---

## Screenshots

![Ollama Model Manager — main window](docs/screenshot-1.png)

![Ollama Model Manager — editor scrolled down to the system prompt and actions](docs/screenshot-2.png)

---

## Features

- **Model browser** — name, ID, size and last‑modified date for every installed model.
- **Parameter editor** with tooltips for each option:
  - `num_ctx` — entered in **K** (1K = 1024 tokens) with a slider + spinbox, hard‑capped at the model's maximum context.
  - `temperature`, `top_p`, `top_k`
  - `repeat_penalty`, `repeat_last_n`, `presence_penalty`, `frequency_penalty`
  - custom `SYSTEM` prompt
- **Create / Update models** — generate a `Modelfile` and run `ollama create` for you.
- **Modelfile preview** — inspect exactly what will be generated before applying.
- **Rename** (via `ollama cp`) and **delete**, with awareness of shared weight blobs (warns before deleting the last reference).
- **Import GGUF** — turn a local `.gguf` file into an Ollama model.
- **Capability‑safe edits** — vision projector and template layers are inherited, not lost.
- **Localization** — runtime-loaded `.ini` language files (English & Russian included); add a language by dropping a file in `languages/`.
- **API tokens + built-in auth proxy** — issue and revoke one token per device (laptop, phone, …)
  from **Tools → API tokens…**. Tokens live **only in KDE Wallet** and are checked by the proxy
  inside this application: they are never written to disk, and revocation takes effect on the very
  next request.
- **Daemon & tray** — optionally runs in the background with a tray icon, proxying Ollama on the
  LAN while the GUI window stays hidden.

---

## Requirements

- [Ollama](https://ollama.com) installed and available in `PATH`
- **Qt 6** (Widgets, Core, Network)
- **KDE Frameworks 6 — KWallet** (`KF6::Wallet`, plus `extra-cmake-modules`) for the token manager
- **CMake ≥ 3.16**
- **OpenSSL** CLI (≥ 3.0) at runtime — used to generate and renew the internal CA/certificate
- A C++17 compiler (GCC / Clang)

On Debian/Ubuntu:

```bash
sudo apt install build-essential cmake qt6-base-dev
```

On Fedora:

```bash
sudo dnf install gcc-c++ cmake qt6-qtbase-devel
```

On Arch:

```bash
sudo pacman -S base-devel cmake qt6-base kwallet extra-cmake-modules
```

---

## Build

```bash
git clone https://github.com/ur3ley/ollama-model-manager.git
cd ollama-model-manager
cmake -B build
cmake --build build
```

The binary is produced at `build/ollama-model-manager`.

Optionally install it system‑wide:

```bash
sudo cmake --install build
```

---

## Usage

```bash
./build/ollama-model-manager
```

1. The left panel lists your installed models. Select one to load its parameters into the editor.
2. Adjust parameters. Use **Preview** to see the generated `Modelfile`.
3. **Update** rebuilds the selected model in place; change the **Name** field and press **Create** to save a new variant instead.
4. **Import GGUF** (Ctrl+I) creates a model from a local `.gguf` file.

Keyboard shortcuts: `Ctrl+N` new model · `Ctrl+I` import GGUF · `F5` refresh · `Ctrl+Q` quit.

> The interface language can be changed at runtime via the **Language** menu.

---

## Built-in auth proxy

The manager itself can act as a token-checking reverse proxy in front of Ollama. It replaces the
earlier nginx + podman container (archived in [`legacy/`](legacy/)): no Python script, no tmpfs
snippet, no container reload — tokens are read from KDE Wallet into process memory and looked up
by SHA-256, so plaintext tokens never leave the wallet.

Two listen modes (**Settings → Proxy**):

| Mode | Address | Port |
|---|---|---|
| Selected interfaces | one or more concrete interface addresses | may be the same as Ollama's (`11434`) |
| All interfaces | `0.0.0.0` | must differ from Ollama's port (default `11435`) |

The reason is a kernel rule: `0.0.0.0:P` cannot be bound while Ollama holds `127.0.0.1:P`, whereas
`192.0.2.10:P` can. Loopback is never bound, so local clients (`ollama` CLI, the manager itself)
keep talking to Ollama directly, without a token.

Daemon mode:

```bash
ollama-model-manager --daemon --listen 192.0.2.10:11434
ollama-model-manager --daemon --listen 0.0.0.0:11435
ollama-model-manager --show     # raise the window of a running instance
```

Streaming is preserved byte-for-byte (`/api/chat` NDJSON, `/v1` SSE). Full architecture, limits and
the migration/rollback procedure: [`docs/proxy.md`](docs/proxy.md).

### TLS on the internal hop

The OPNsense → host leg can be encrypted as well. The default mode is an **internal CA**:

- a 10‑year Root CA is generated once (`~/.config/ollama-manager/tls/`, mode 0700/0600);
- a 90‑day leaf certificate (`SAN IP:192.0.2.10`, `DNS:ollama.example.org`) is renewed
  automatically 30 days before expiry;
- import **only the CA** into OPNsense once (*TLS: Trusted Certificate*), leave
  *Verify Certificate* on — leaf renewals need no further imports.

Alternative modes: your own cert/key files, or a publicly trusted Let's Encrypt certificate
obtained via deSEC DNS‑01 (`lego`). Keys never leave `~/.config/ollama-manager/tls/`.

### Security

- Tokens never touch disk; they are compared as SHA-256 hashes, so a timing attack cannot
  recover them character by character.
- Request limits: 100 MiB body (counted by actual bytes, chunked included), 32 KiB and 100
  headers, 8 KiB URI, 256 connections (0 = deny all), 10 s header timeout, bidirectional
  backpressure at 4/1 MiB.
- Control characters and obs-fold are rejected; hop-by-hop headers (including those named in
  `Connection`) are stripped; device names are validated before use.
- Parser fuzzed with >4.5 million libFuzzer iterations under ASan/UBSan (zero crashes).
- A hardened systemd unit is shipped in `packaging/systemd/`.

## API tokens

| Piece | Value |
|---|---|
| Wallet | `kdewallet`, folder `ollama-auth` |
| Keys | `<device>` = token, `created:<device>` = ISO-8601 date |
| Token format | 24 random bytes → 48 hex characters |
| Endpoint | `https://ollama.example.org` |

Workflow: **Tools → API tokens… → Issue token** → type a device name → the token is shown once in a
modal window (selectable, copied only by an explicit button; it never leaves the wallet in plaintext
afterwards; **Show** reveals it on demand). **Revoke** removes it from the wallet, and the device
loses access immediately. **Copy URL** puts the endpoint on the clipboard.

Manual smoke tests:

```bash
./build/token-selftest   # wallet: list → issue → read → revoke (needs an unlocked wallet)
./build/proxy-selftest   # proxy: auth, header rewriting, streaming, chunked bodies
```

## Localization

The UI text lives in plain `.ini` files under `languages/`, one file per
language named by its code (`en.ini`, `ru.ini`, `et.ini`, …). English
(`en.ini`) is the base and the fallback.

Adding a language requires **no recompilation**:

1. Copy `languages/en.ini` to `languages/<code>.ini`.
2. Set the display name under `[meta] name = …` and translate the right-hand
   side of each `key = value` line (keep keys, `%1`/`%2` placeholders and `\n`).
3. Save as UTF-8 and restart — the language appears automatically in the
   **Language** menu, which is built by scanning the folder.

See [`languages/_HOWTO.txt`](languages/_HOWTO.txt) for full instructions.
The chosen language is remembered between sessions.

---

## How it works

Ollama stores each model as a *manifest* pointing to separate layers (weights, projector/`mmproj`, template, parameters, license). Several tags can share the same large weight blob while carrying their own small parameter layers.

When you rebuild a model, the generated `Modelfile` uses:

```
FROM <model-name>
PARAMETER num_ctx 131072
...
```

Because `FROM` references the model **by name**, Ollama inherits **all** of its layers — including the vision projector and chat template. Rebuilding `FROM /var/lib/ollama/blobs/sha256-…` (the raw weights blob) would keep only the weights and silently strip multimodality; this tool deliberately avoids that.

---

## Limitations

- **Audio is not supported.** Ollama's runtime only wires up image projectors (CLIP‑style); even models that natively support audio expose only text + `vision` in Ollama. This is an Ollama limitation, not a limitation of this tool.
- `think` / reasoning level cannot be set via `Modelfile` — use `ollama run --think` or the API.
- Blob paths in the app assume the default Ollama storage layout (`/var/lib/ollama/blobs`).

---

## License

This project is released under the [MIT License](LICENSE) — you are free to use, modify and distribute it, including commercially.

**Third‑party components:** this application links against **Qt 6**, which is distributed under the GNU LGPL v3 (and other licenses). Qt itself remains under its own license; the MIT license here applies only to this project's source code. If you redistribute a binary, comply with Qt's LGPL terms (e.g. allow relinking against Qt).

---

## Contributing

Issues and pull requests are welcome. Please keep the existing code style (Qt/C++17, no comments unless necessary) and describe the motivation for changes.
