# Legacy: контейнерный auth-прокси (nginx + podman)

Сюда перенесена старая схема авторизации, которая работала до встроенного прокси.
Каталог нужен для истории и отката; в сборке не участвует.

## Как было устроено

```
клиент → https://ollama.ur3ley.dedyn.io (TLS на роутере, 443)
       → 192.168.1.2:11435 (podman publish, pasta)
       → nginx в контейнере ollama-auth
       → host.containers.internal:11434 (Ollama на хосте)
```

- `nginx/nginx.conf` — конфиг контейнера: проверка `Authorization: Bearer` и
  `X-Api-Key` через `map`, карта токенов подключалась из
  `/etc/nginx/tokens/maps.conf` (tmpfs).
- Токены выпускал `ollama-model-manager` (KDE Wallet, папка `ollama-auth`),
  а `ollama-tokens-sync.py` (теперь здесь, в `legacy/`) материализовал их в
  `maps.conf` и делал `podman exec ollama-auth nginx -s reload`.
- `systemd/` — пользовательские юниты старой схемы
  (`ollama-auth.service`, `ollama-auth-tokens.{service,timer}`).

## Откат на старую схему

1. `mkdir -p ~/projects/ollama-auth`
2. `cp legacy/nginx/nginx.conf ~/projects/ollama-auth/nginx.conf`
3. Вернуть скрипт синхронизации: `mkdir -p scripts && cp legacy/ollama-tokens-sync.py scripts/`
4. `cp legacy/systemd/*.service legacy/systemd/*.timer ~/.config/systemd/user/`
5. `systemctl --user daemon-reload`
5. `podman start ollama-auth` (контейнер не удалялся) либо пересоздать:
   `podman run -d --name ollama-auth -p 11435:11435 \
      -v ~/projects/ollama-auth/nginx.conf:/etc/nginx/conf.d/default.conf:ro,Z \
      -v /run/user/1000/ollama-auth:/etc/nginx/tokens:ro,Z \
      docker.io/library/nginx:alpine`
6. `systemctl --user enable --now ollama-auth.service ollama-auth-tokens.timer`
7. Вернуть правило на роутере: `443 → 192.168.1.2:11435`.
