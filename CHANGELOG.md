# Changelog

All notable changes to this project are documented here.
This project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
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

## [1.0.0]

### Added
- Model browser (name, ID, size, modified date).
- Parameter editor: `num_ctx`, `temperature`, `top_p`, `top_k`,
  `repeat_penalty`, `repeat_last_n`, `presence_penalty`, `frequency_penalty`,
  and a custom `SYSTEM` prompt.
- Create, update, rename (`ollama cp`) and delete models.
- `Modelfile` preview before applying.
- GGUF import.
