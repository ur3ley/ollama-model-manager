# State — Ollama Model Manager

## Project Info

- Qt6 (C++17) приложение для управления моделями Ollama
- Сборка: CMake + Make
- Бинарник: `build/ollama-model-manager`

## Current Status
(2025-05-29)

- Добавлен параметр `think` в редактор моделей (секция "Рассуждения") с выбором уровня: true/false/high/medium/low
- Параметр `PARAMETER think` записывается в Modelfile при включенном чекбоксе
- Секция видна всегда, независимо от модели

## Known Issues

- 

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
