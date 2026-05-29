# State — Ollama Model Manager

## Project Info

- Qt6 (C++17) приложение для управления моделями Ollama
- Сборка: CMake + Make
- Бинарник: `build/ollama-model-manager`

## Current Status
(2025-05-29)

- Параметр `think` в редакторе моделей **удалён** — Ollama 0.24 не поддерживает `PARAMETER think` в Modelfile
- `--think false/high/medium/low` работает только как флаг `ollama run`, не как PARAMETER

## Known Issues

- Невозможно управлять рассуждениями (reasoning/thinking) через Modelfile — только через `ollama run --think` или open-webui

## Key Paths

| Path | Purpose |
|------|---------|
| src/main.cpp | Точка входа |
| src/mainwindow.cpp/h | Главное окно |
| src/modeleditorwidget.cpp/h | Редактор параметров модели |
| src/modelfilebuilder.cpp/h | Генератор Modelfile |
| src/ollamacli.cpp/h | Обёртка над ollama CLI |
| src/modellistwidget.cpp/h | Список моделей |
| src/ggufimportdialog.cpp/h | Импорт GGUF |
| src/renamedialog.cpp/h | Диалог переименования |
