# Аудит-артефакты прокси (этап D1)

Скрипты/цели, которыми проверялся отчёт
[`tasks/2026-11-20-security-audit.md`](../../tasks/2026-11-20-security-audit.md).
В сборку проекта (`CMakeLists.txt`) **не входят** — собираются вручную, чтобы не менять
продуктовый билд. Требуют Qt 6.12 (Core+Network), `clang++` (libFuzzer/ASan), `openssl`.

## Общая подготовка

```bash
cd <repo>
mkdir -p /tmp/opencode/audit
QT_CORE_CFLAGS="$(pkg-config --cflags Qt6Core)"
QT_NET_CFLAGS="$(pkg-config --cflags Qt6Core Qt6Network)"
QT_NET_LIBS="$(pkg-config --libs Qt6Core Qt6Network)"
MOC=build/proxy-selftest_autogen/UVLADIE3JM   # moc_ollamaproxy.cpp / moc_tlsmanager.cpp
```

ASan-сборка всего проекта (для `proxy-selftest` под санитайзерами):

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build-asan -j$(nproc)
ASAN_OPTIONS=detect_leaks=0 ./build-asan/proxy-selftest
```

## 1. `qssl-threshold.cpp` — порог 30 у `QSslServer`

Голый `QSslServer` + тривиальный обработчик, без кода продукта. Показывает, что
`QTcpServer::maxPendingConnections()` (30) блокирует handshake, и что
`setMaxPendingConnections(1000)` это лечит.

```bash
openssl req -x509 -newkey rsa:2048 -nodes -keyout /tmp/opencode/audit/t.key \
  -out /tmp/opencode/audit/t.crt -days 2 -subj "/CN=proxy.test" \
  -addext "subjectAltName=DNS:proxy.test,IP:127.0.0.1"
g++ -std=c++17 -O1 -g -fPIC -I src $QT_NET_CFLAGS tests/audit/qssl-threshold.cpp \
  -o /tmp/opencode/audit/qssl-threshold $QT_NET_LIBS
/tmp/opencode/audit/qssl-threshold /tmp/opencode/audit/t.crt /tmp/opencode/audit/t.key 0 25 30 40
OMM_MAXPENDING=1000 /tmp/opencode/audit/qssl-threshold /tmp/opencode/audit/t.crt \
  /tmp/opencode/audit/t.key 30 100
```

## 2. `tls-stress.cpp` — жизненный цикл соединений

```bash
g++ -std=c++17 -g -O1 -fPIC -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I src -I build/proxy-selftest_autogen/include $QT_NET_CFLAGS \
  tests/audit/tls-stress.cpp src/ollamaproxy.cpp src/proxyhead.cpp src/tokenmap.cpp \
  src/tlsmanager.cpp $MOC/moc_ollamaproxy.cpp $MOC/moc_tlsmanager.cpp \
  -o /tmp/opencode/audit/tls-stress $QT_NET_LIBS

export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 MALLOC_CHECK_=3
for m in "seq 100" "perround 30" "killmid 10" "concurrent 25" \
         "abortplain 1000" "aborttls 500" "reject 6"; do
  /tmp/opencode/audit/tls-stress $m
done
```

`fdsweep` — сколько висящих TCP ломают TLS; `slots`/`slotsplain` — занятие слотов молчанием (TLS/plain);
`fds` — сколько fd живёт мимо `maxConnections`.

## 3. `tls-inproc-repro.cpp` — заявленный heap-баг C1

```bash
g++ -std=c++17 -g -O1 -fPIC -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I src -I build/proxy-selftest_autogen/include $QT_NET_CFLAGS \
  tests/audit/tls-inproc-repro.cpp src/ollamaproxy.cpp src/proxyhead.cpp src/tokenmap.cpp \
  src/tlsmanager.cpp $MOC/moc_ollamaproxy.cpp $MOC/moc_tlsmanager.cpp \
  -o /tmp/opencode/audit/repro-asan $QT_NET_LIBS

ASAN_OPTIONS=detect_leaks=0 MALLOC_CHECK_=3 /tmp/opencode/audit/repro-asan proxy 30
ASAN_OPTIONS=detect_leaks=0 MALLOC_CHECK_=3 /tmp/opencode/audit/repro-asan proxy-per-round 30
```

Прочие режимы: `child` (клиент дочерним процессом — как в selftest), `mix`
(хороший/без CA/мусор/обрыв), `kill-mid`.

## 4. `backpressure.cpp` — DoS телом запроса

```bash
g++ -std=c++17 -O1 -g -fPIC -I src -I build/proxy-selftest_autogen/include $QT_NET_CFLAGS \
  tests/audit/backpressure.cpp src/ollamaproxy.cpp src/proxyhead.cpp src/tokenmap.cpp \
  $MOC/moc_ollamaproxy.cpp -o /tmp/opencode/audit/backpressure $QT_NET_LIBS
/tmp/opencode/audit/backpressure 400   # RESULT sent=400 MiB rss_growth=401 MiB
```

## 5. `parser-cases.cpp` — поведение парсера на пограничных входах

```bash
g++ -std=c++17 -O1 -g -fPIC -I src $QT_CORE_CFLAGS tests/audit/parser-cases.cpp \
  src/proxyhead.cpp -o /tmp/opencode/audit/parser-cases $(pkg-config --libs Qt6Core)
/tmp/opencode/audit/parser-cases > tasks/audit/parser-cases.txt
```

## 6. Фаззинг

```bash
clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
  -fPIC -I src $QT_CORE_CFLAGS tests/audit/fuzz-head.cpp src/proxyhead.cpp \
  -o /tmp/opencode/audit/fuzz-head $(pkg-config --libs Qt6Core)

cp tests/audit/corpus-head/* /tmp/opencode/audit/corpus-run/
ASAN_OPTIONS=detect_leaks=0 /tmp/opencode/audit/fuzz-head /tmp/opencode/audit/corpus-run \
  -max_total_time=120 -print_final_stats=1
```

Семантический оракул (тот же бинарь; трап на управляющие символы и пустые имена
в том, что уйдёт апстриму — это не порча памяти, а подтверждение M-3/M-4):

```bash
OMM_FUZZ_STRICT=1 /tmp/opencode/audit/fuzz-head /tmp/opencode/audit/corpus-run \
  -max_total_time=45 -artifact_prefix=/tmp/opencode/audit/strict-
```

## 7. `-Wconversion` по парсеру

```bash
g++ -std=c++17 -O2 $(pkg-config --cflags Qt6Core | sed 's/-I/-isystem /g') -I src \
  -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wpedantic \
  -c src/proxyhead.cpp -o /dev/null
```

## 8. Ожидания после фиксов 2026-11-20 (H-2/H-1)

`tls-stress`: `reject`/`slots` теперь ждут **закрытия до handshake** (лимит
считается на accept, ответить 503 в TLS-плече нечем); `fds` проверяет, что 50
висящих TCP не оставляют серверных fd сверх `maxConnections`; `fdsweep` —
что при 20…100 висящих соединениях легитимный клиент получает 200.

`backpressure`: `rss_growth` теперь ~4 МиБ (high-water backpressure), а не
`sent`; при апстриме, который читает, лишние байты тела дают 413.

`parser-cases`: все управляющие символы/obs-fold/дубликаты CL/TE+CL дают
400/413/414/431/501 (см. `tasks/audit/parser-cases.txt`).
