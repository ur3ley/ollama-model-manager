# Rollback Journal

## 2025-05-29 — Добавлен параметр think

**Что сделано:**
- `modelfilebuilder.h`: Добавлены поля `enableReasoning` (bool) и `reasonValue` (QString) в Config
- `modelfilebuilder.cpp`: Добавлена запись `PARAMETER think <value>` при enableReasoning=true
- `modeleditorwidget.h`: Добавлены `m_reasoningGroup`, `m_reasoningCheck`, `m_reasoningCombo`, `m_baseCapabilities`
- `modeleditorwidget.cpp`: Секция "Рассуждения (think)" всегда видна

**Как откатить:**
1. Убрать блок `m_reasoningGroup` из `setupUI()` в modeleditorwidget.cpp
2. Убрать строки `config.enableReasoning` и `config.reasonValue` из `collectConfig()`
3. Убрать загрузку `think` параметра из `loadModel()`
4. Убрать сброс reasoning из `setCreateMode()`
5. Убрать поля из `modelfilebuilder.h` и `modeleditorwidget.h`
6. Убрать `PARAMETER think` из `modelfilebuilder.cpp`
