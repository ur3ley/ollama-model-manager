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

---

## Requirements

- [Ollama](https://ollama.com) installed and available in `PATH`
- **Qt 6** (Widgets, Core)
- **CMake ≥ 3.16**
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
sudo pacman -S base-devel cmake qt6-base
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
