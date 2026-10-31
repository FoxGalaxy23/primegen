# NAL — сетевой слой

## Linux: libcurl-транспорт с собственным HTTP/1.1 fallback

В Linux-сборке, если `pkg-config` находит `libcurl`, основной сетевой транспорт использует libcurl и согласует HTTP/2 по TLS ALPN;
HTTP/3 включается только в сборках libcurl с QUIC/HTTP3. Brotli (и zstd, если собраны в libcurl) распаковываются автоматически.
Системные переменные прокси libcurl (`HTTP_PROXY`, `HTTPS_PROXY`, `ALL_PROXY`, `NO_PROXY`) поддержаны; `MLBROWSER_PROXY` и
`MLBROWSER_NOPROXY` позволяют переопределить прокси для MLBrowser. Заголовок `Range` проходит к серверу как обычный заголовок запроса.
`MLBROWSER_NET=native` отключает libcurl и принудительно включает собственный HTTP/1.1-клиент из `mods/nal/common/`:


| Файл | Назначение |
|---|---|
| `nhttp.c/.h` | DNS, TCP с таймаутами, keep-alive пул, HTTP/1.1 (chunked, Content-Length, до закрытия), gzip/deflate, редиректы (301/302/303/307/308), куки RFC 6265 |
| `ntls.h` | Интерфейс TLS-клиента (рукопожатие, чтение, запись, закрытие) — **подменяемый** |
| `ntls_openssl.c` | Реализация интерфейса на системном OpenSSL 1.1/3.x через `dlopen` (заголовки при сборке не нужны) |
| `ninflate.c/.h` | Собственная распаковка DEFLATE / gzip (CRC-32) / zlib (Adler-32) |

Проверка сертификатов обязательна: цепочка по системному хранилищу CA (учитывает `SSL_CERT_FILE` / `SSL_CERT_DIR`), имя хоста
или IP по SAN, минимум TLS 1.2. Небезопасного режима нет.

### Как это встроено
- `nal_http_get` / `nal_download_file` — синхронно в вызывающем процессе. Куки: БД → Netscape-файл → jar в памяти → в БД
  возвращаются только изменённые (в том числе HttpOnly — раньше они терялись).
- `nal_async_poll_ex` — как `nal_async_poll`, но возвращает и длину тела (тело бинарное, может содержать `\0`); JS получает его как `ArrayBuffer`.
- `nal_async_*` — как и раньше `fork()`: потомок выполняет запрос через выбранный `nhttp`-транспорт и пишет результат в те же файлы; родитель
  опрашивает `waitpid(WNOHANG)`. В файл заголовков попадает только финальный блок, поэтому `fetch()` теперь видит верный статус
  после редиректа (раньше брался статус первого блока — 302).
- Пул соединений живёт в процессе. Соединение, унаследованное потомком через `fork`, не используется (проверка по pid) и
  закрывается без `close_notify`, чтобы не испортить сессию родителя.
- Если libcurl не найдена при сборке, используется собственный HTTP/1.1 backend. `MLBROWSER_NET=native` принудительно выбирает его даже при наличии libcurl.
- `nal_download_start` запускает управляемую загрузку в отдельном процессе; `nal_download_pause`, `nal_download_continue`, `nal_download_cancel`, `nal_download_poll` управляют жизненным циклом, а `nal_download_resume_start` асинхронно ставит на продолжение проверенную частичную загрузку. Этот менеджер на данный момент реализован на Linux.
- Заголовки ответа для управляемых загрузок перехватываются до записи тела: сохраняются Content-Length, ETag, Last-Modified и Accept-Ranges в боковой файл, а родитель импортирует их в SQLite. Resume добавляет `Range: bytes=N-`, `If-Range` (сильный ETag, иначе Last-Modified) и `Accept-Encoding: identity`, принимает только согласованный `206`/`Content-Range` и сверяет итоговый размер. Если условия не соблюдены — частичный файл сохраняется, но загрузка не считается завершённой.
- Управляемые загрузки требуют native NHttp transport. При `MLBROWSER_NET=curl` менеджер отказывает в запуске, потому что CLI fallback не предоставляет нужный callback до начала записи тела. `nal_download_file` — прежний синхронный API и не является частью фонового менеджера.

### Заменить TLS на свой
Достаточно реализовать функции из `ntls.h` (`ntls_available`, `ntls_connect`, `ntls_read`, `ntls_write`, `ntls_close`) и подставить
файл в `mods/nal/linux/module.mk` вместо `ntls_openssl.c`. HTTP-слой менять не нужно.

### HTTP-кэш
Приватный кэш на диске (`cache/http/<хэш url>.e`, до 256 МиБ, вытеснение по давности использования; отключить — `MLBROWSER_NO_CACHE=1`).
Работает для GET без тела и без пользовательских заголовков (страницы, CSS, JS; `fetch` с собственными заголовками и скачивание файлов в обход кэша).
- **Свежесть** (RFC 9111): `Cache-Control: max-age` / `immutable`, `Expires`, `Age`; эвристика 10 % от возраста `Last-Modified` (не больше суток; для URL с `?` без явной свежести не применяется).
- **Перепроверка:** устаревшая запись с `ETag`/`Last-Modified` проверяется условным запросом; `304` обновляет свежесть, `200` заменяет запись.
- **Не кэшируются:** `no-store`, ответы с `Vary` по чему-либо, кроме `Accept*`/`User-Agent` (например `Vary: Cookie`), ответы без свежести и без валидатора.
  `Set-Cookie` из ответа, взятого из кэша, повторно не применяется. Небезопасный метод (POST/PUT/…) к тому же URL удаляет запись.
- **Политики** (`nal_cache_policy`): `NORMAL`; `RELOAD` — кнопка «Обновить» (кэшу не доверяем, но валидаторы используются); `OFFLINE` — отдавать записи даже устаревшими.
- Записи пишутся через временный файл и `rename` (безопасно при параллельных процессах); повреждённые файлы распознаются и удаляются.

### Безопасность (источники, CORS, смешанное содержимое)
Запросы из скриптов страницы (`fetch`, `XMLHttpRequest`) идут через `ncors.c` — политика действует в нативном коде и не зависит от JS-обёрток:
- **Одного источника / CORS** (Fetch Standard): `Origin`, `Referer` (по `strict-origin-when-cross-origin`), `Sec-Fetch-Site/Mode/Dest`; «запрещённые» заголовки от скрипта
  (`Cookie`, `Origin`, `Host`, `Sec-*`, …) отбрасываются; preflight (`OPTIONS`) для не «простых» запросов с проверкой `Access-Control-Allow-Methods/Headers/Origin/Credentials`
  и дисковым кэшем результата (`Access-Control-Max-Age`); межсайтовый ответ без разрешения недоступен скрипту (для него — сетевая ошибка, как в браузерах).
- **Куки и credentials:** `omit` — без куки; `same-origin` (умолчание) — только на свой источник; `include` — и на чужой, но тогда нужен `Access-Control-Allow-Credentials: true`
  и точный `Access-Control-Allow-Origin` (не `*`). Куки из ответа сохраняются только если политика разрешила их использовать. `Set-Cookie` скрыт от скрипта всегда;
  межсайтовые ответы показывают только безопасные заголовки и перечисленные в `Access-Control-Expose-Headers`.
- **Смешанное содержимое:** страница по https не загружает по http скрипты, стили, `@import`, шрифты, воркеры, `fetch` и `ws://` (loopback — localhost/127.x/::1 — разрешён для разработки).
  При использовании libcurl сетевой ответ всё равно проходит через общую политику HSTS/CSP и управление cookies NAL; автоматическое следование редиректам libcurl отключено, чтобы редиректы проходили через браузерную проверку.
- **Public Suffix List** (`psl.c`, данные `psl_data.inc` генерирует `tools/gen_psl.py`): правила обновлены до снимка, поставляемого с локальным OpenJDK 21 (9 762 правила, включая private domains). В исходном файле нет upstream-даты, поэтому это не гарантированно самый свежий список. Куки с `Domain=com`/`co.uk`/`github.io` отвергаются, «тот же сайт» определяется по eTLD+1.
- **IDN/Punycode:** UTF-8 имена хостов приводятся к ASCII/Punycode в URL-клиенте и security-политике. Реализация проверяет UTF-8, метки DNS и опасные управляющие кодовые точки, но не является полной реализацией UTS #46/полного IDNA2008 mapping.
- **CSP:** поддержан базовый `Content-Security-Policy` для `default-src`, скриптов, стилей, изображений, шрифтов, подключений и workers; учитываются `'self'`, `'none'`, схемы, домены, wildcard, порты, пути, `nonce-*` и `unsafe-inline`. Политика также блокирует часть inline-обработчиков `onclick`. Это пока подмножество CSP, не полный стандарт: не поддержаны `report-only`, meta CSP, хэши inline-кода, `strict-dynamic`, все `*-src-attr` нюансы и полный enforcement `unsafe-eval`.
- **HSTS:** `Strict-Transport-Security` на HTTPS-ответе сохраняется в `cache/security/hsts.txt`; `max-age`, `includeSubDomains`, истечение и апгрейд HTTP/80 → HTTPS поддержаны. Это локальная база браузерного профиля; preload-лист HSTS пока отсутствует.
- **SameSite:** `Strict`, `Lax` (значение по умолчанию для старых cookie) и `None` реализованы в собственном Linux cookie jar; `SameSite=None` без `Secure` отклоняется. Движок пока не полностью реализует все нюансы современных алгоритмов cookie/навигации.
- **WebSocket:** на Linux есть проверка CSP `connect-src`, mixed content, HSTS-apgrade `ws://` → `wss://`, TLS через системный OpenSSL с верификацией сертификата и валидация `Sec-WebSocket-Accept`. Windows-реализация WebSocket пока fail-closed и соединения не устанавливает.
- **Песочница:** Linux worker-процессы пытаются включить user/mount/IPC/UTS namespaces и ставят seccomp-фильтр, запрещающий запуск новых процессов, ptrace, монтирование/смену namespaces и ряд опасных kernel API. Это defense-in-depth deny-list, а не полная sandbox: нет отдельного сетевого namespace, файлового профиля/AppArmor/SELinux и строгой allow-list всех syscall. При отказе установки seccomp worker отказывает в запуске.
Проверки: `bash tests/net/cors.sh` (38 сценариев), `bash tests/net/psl.sh`.

### Сеть и ограничения
- HTTP/2, Brotli и прокси доступны только в сборке с соответствующими возможностями libcurl. HTTP/3 не гарантирован: он требует libcurl, собранную с QUIC/HTTP3 (например, ngtcp2 или quiche); в противном случае HTTPS автоматически использует HTTP/2/HTTP/1.1.
- `Range` заголовки можно передавать через Fetch/XHR и сетевой API; для запросов с Range клиент предпочитает `Accept-Encoding: identity`, чтобы автоматическая распаковка не нарушала byte offsets. CORS считает безопасным только синтаксически корректный одиночный `bytes=N-M`/`bytes=N-`; multi-range и суффикс-range требуют preflight. Управляемые загрузки и безопасное возобновление реализованы на Linux; детали и ограничения приведены в секции «Download manager» ниже.
- `XMLHttpRequest.open(method, url, false)` работает синхронно в Linux NAL; не поддерживает `timeout` и не-текстовый `responseType`, что вызывает `InvalidAccessError`. Win32 выполняет sync XHR и POST через WinHTTP, но cross-origin sync XHR закрыт fail-closed до полноценного CORS preflight; Windows-сборка в этом окружении не проверялась.
- Обычные HTML-формы поддерживают GET/POST, `application/x-www-form-urlencoded`, `text/plain` и `multipart/form-data`; нативный file picker передаёт выбранные файлы с лимитом 32 МиБ на файл и 64 МиБ на запрос. JS `FormData`/XHR уже умеет формировать бинарное multipart-тело.
- Собственный backend без libcurl остаётся HTTP/1.1 + gzip/deflate. Кэш без `Vary`-ключей, `stale-while-revalidate`/`stale-if-error`; CSP и IDN реализованы как совместимое подмножество и требуют дальнейших тестов на совместимость с веб-стандартами. Windows (`nal_win32`) использует WinHTTP; cookie/SameSite и WebSocket на Win32 не доведены до паритета с Linux, а в текущем окружении MinGW не установлен, поэтому Windows-сборка здесь не подтверждена.

### Проверка
`bash tests/net/security.sh` — тесты Punycode, CSP nonce/source matching, HSTS, mixed-content, защиты loopback-проверки от поддельных префиксов и smoke-тест seccomp (запрет `fork`). `bash tests/net/curl_transport.sh` запускает автономные локальные проверки libcurl-транспорта (Range/identity encoding, HEAD без тела, Brotli, proxy, multipart и заголовки до чтения тела; пропускается, если libcurl dev package отсутствует). `tests/net/run.sh` также проверяет SameSite Strict/Lax и отказ принимать SameSite=None без Secure.
`bash tests/net/cache.sh` — 23 проверки кэша (свежесть, 304, no-store, Vary, gzip, reload/offline, инвалидация POST).
`bash tests/net/run.sh` — локальные HTTP/HTTPS-серверы, тестовый CA, 48 сценариев (chunked, gzip, редиректы, SameSite cookies, keep-alive,
верификация сертификатов, таймауты, обрывы); `NET_REAL=1` добавляет реальные сайты. Утилита `tools/nhttp_cli.c` — клиент для ручных проверок.


### Download manager (2026-10, Linux)

The managed-download controller is connected to explicit `<a download>` link activation. It stores jobs in SQLite, saves response validators in an atomically written sidecar, tracks bytes written, and exposes pause/continue/cancel/resume actions on `browser://downloads`. The page is refreshed approximately once per second while active. Stale jobs are reconciled at startup from the partial file and metadata. Resume is intentionally fail-closed: it requires a consistent partial file, byte-range support, and a strong ETag or `Last-Modified`; the response must be `206 Partial Content` with a matching `Content-Range` and validator, and the final byte count must match the advertised total when known. This validates transfer continuity/length, not a cryptographic digest. The managed downloader currently requires the Linux native NHttp transport; it declines managed downloads when `MLBROWSER_NET=curl` selects the CLI fallback because that path does not expose the pre-body validation callback. Automatic capture of arbitrary navigation responses with `Content-Disposition: attachment`, a native save-path dialog, and a full Windows implementation remain future work.

Regression checks (Linux; dependencies include GCC, SQLite, OpenSSL and libcurl development files):

```sh
make -j4
python3 tests/downloads/controller_contract_test.py
python3 tests/downloads/schema_resume_test.py
python3 tests/downloads/transfer_integration_server.py
```

The transfer integration test uses a deterministic local HTTP server and exercises real NHttp transfer code for validator persistence, pause/continue, cancel/resume, and stale-job recovery. It verifies the final file byte-for-byte against deterministic test content.

`browser://downloads` also escapes filename, URL and status values before inserting them into HTML, limits history to the newest 500 entries, and grows its output buffer safely up to 1 MiB to avoid stored markup/script injection and fixed-buffer overflow.
