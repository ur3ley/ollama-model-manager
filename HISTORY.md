# History — Ollama Model Manager

## Change Log

### 2026-07-08 — Фикс перекрёстного отображения num_ctx (соседка по blob)

- Проблема: у моделей с общим blob весов (напр. justingtzk 128K/256K) поле «по умолчанию
  у базовой модели» показывало num_ctx СОСЕДНЕЙ модели (крест-накрест)
- Причина: FROM определялся через findOtherModelSharingBlob → бралась любая модель с тем же
  blob весов (по кратчайшему имени), и updateContextInfo читал `ollama show` для неё
- Фикс: в loadModel FROM теперь = собственное имя открытой модели (modelName) напрямую,
  fromBase/realFrom/blob оставлены только как fallback. Поле показывает дефолт самой модели
- setEditMode вызывается до updateContextInfo

### 2026-07-08 — Фикс потери мультимодальности при редактировании

- Проблема: при изменении параметров (напр. num_ctx) модель теряла vision-возможность
- Причина: в loadModel (modeleditorwidget.cpp) в поле FROM подставлялся путь к blob весов
  (`m_fromBlob`) с приоритетом над именем модели. `ollama create FROM /blob` наследует
  только веса — слой проектора (mmproj), TEMPLATE, RENDERER, PARSER теряются
- Фикс: изменён порядок приоритетов — `params.fromBase` (имя модели из комментария
  `# FROM ...`) теперь выше `m_fromBlob`. При FROM=<имя> ollama наследует все слои
- Проверено: `FROM justingtzk/gemma-4...:128K` + PARAMETER num_ctx → vision сохраняется
- Сломанные ранее модели восстанавливаются через `ollama pull <имя>`
- Переименование (`ollama cp`) зрение не ломает — копирует манифест целиком

### 2025-05-29 — Добавлен параметр reasoning/think

- Добавлена секция "Рассуждения (think)" в ModelEditorWidget
- ComboBox с выбором уровня: true, false, high, medium, low
- Чекбокс включает/выключает запись PARAMETER think в Modelfile
- Секция всегда видна (без проверки capability)
- Инициализирован git, созданы STATE.md, HISTORY.md, JOURNAL.md

### 2025-05-28 — Создание проекта

- Начальная версия Ollama Model Manager
- Просмотр, создание, редактирование, переименование, удаление моделей
- Импорт GGUF
