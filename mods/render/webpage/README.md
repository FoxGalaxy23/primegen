# webpage — движок веб-страниц

Конвейер: **HTML → дерево → каскад CSS → раскладка → display list → отрисовка**.
Старый плоский движок (`parse_html` + `layout`) остаётся для интерфейса браузера и служебных страниц;
`layout_and_render()` сам переключается на этот модуль, если корень DOM создан `parse_document()` (`node->v2`).

| файл | что делает |
|---|---|
| `html.c` | токенайзер и построение дерева: raw-text (`style/script/textarea/title/noscript`), сущности, неявные закрытия тегов, кодировки (UTF-8, windows-1251, KOI8-R, latin1), URL-ы |
| `css.c` | цвета, длины и `calc()/min()/max()/clamp()`, `@media`, `@import`, `@font-face` (+`unicode-range`), декларации, селекторы (комбинаторы, `[attr]`, `:nth-child`, `:not/:is/:where`, …) |
| `style.c` | UA-стили, индекс правил, каскад (специфичность, `!important`), `var()`, наследование → `node->st` |
| `layout2.c` (+`l2_flex/grid/table.inc`) | блоки с схлопыванием отступов, строчный поток, float, inline-block, flex, grid (линии, области, auto-flow, span, minmax/fr/auto-fill/auto-fit, выравнивание), таблицы, relative/absolute/fixed |
| `paint.c` | скруглённые рамки с AA, тени, фоновые картинки, текст, формы, hit-test, выделение, GET-отправка форм |
| `fx.c`, `style_fx.inc` | слои эффектов: `filter`, `backdrop-filter`, `clip-path`, `mask`, `transform` (разбор значений и растровые операции) |
| `anim.c` | `transition`, `@keyframes`/`animation`, события `transitionend`/`animationend`, `:hover/:active/:focus` |
| `images.c` | предзагрузка и кэш изображений; `stb_image` для PNG/JPEG/GIF/BMP и опциональные `libwebp`/`libavif` для WebP/AVIF; выбор `srcset`/`sizes` и источников `<picture>` |

Запуск с URL: `./my_browser https://pypi.org/` или `./my_browser /path/to/page.html`.

Не поддерживается: JavaScript-управляемая вёрстка. (Шрифты: WOFF/WOFF2/CFF — см. `../ttf_render/README.md`;
псевдоэлементы `::before/::after` и `var()` работают; `font-weight` учитывается по начертаниям веб-шрифта.)

### Эффекты, transform, transition/animation

- **Слои.** Элемент с `filter`, `backdrop-filter`, `clip-path`, `mask` или `transform` оборачивается в `C_FX … C_ENDFX` (`mate` — парная команда,
  пары пересчитываются после `stk_reorder`; такой элемент — контекст наложения). При отрисовке (`fx_group` в `paint.c`) содержимое рисуется
  дважды — на чёрном и на белом фоне; из пары берётся цвет и альфа (`α = 1 − (B−A)/255`), поэтому все примитивы рисования остаются как есть.
  Далее по порядку: `filter` → `clip-path` → `mask` → композиция (аффинная матрица, билинейно). Вложенность слоёв — до 3.
- **`filter`**: `blur`, `brightness`, `contrast`, `grayscale`, `sepia`, `saturate`, `hue-rotate`, `invert`, `opacity`, `drop-shadow` (цепочки).
  **`backdrop-filter`**: те же функции над уже нарисованным фоном под боксом (с учётом `border-radius`).
- **`clip-path`**: `inset(… round …)`, `circle()`, `ellipse()`, `polygon()` (nonzero/evenodd), позиции `at`, `closest-side/farthest-side`.
- **`mask`/`mask-image`/`-size`/`-position`/`-repeat`**: `linear-gradient`, `radial-gradient`, `url()` (по альфа-каналу); префикс `-webkit-` принимается.
- **`transform`/`transform-origin`**: `translate*`, `scale*`, `rotate*`, `skew*`, `matrix`; чистый перенос сдвигает `Frame` (без слоя), остальное — слой с
  матрицей. Hit-test учитывает обратную матрицу (`Frame.im`). `rotateX/Y` — приближение масштабом по `cos`.
- **`transition`** (`property/duration/delay/timing-function`, `all`, списки) и **`animation`** (`name/duration/delay/timing/iteration-count/direction/fill-mode/play-state`,
  до 3 одновременных) + `@keyframes` (`from/to/%`, несколько смещений в блоке). Временные функции: `ease*`, `linear`, `cubic-bezier()`, `steps()`.
  Базовый стиль даёт каскад, `anim_step()` перед раскладкой накладывает значения на текущее время (`n->st` меняется на месте); переход стартует в
  `anim_cascade_end()`, если после перекаскада значение изменилось. Анимируются: `opacity`, цвета, размеры, `margin/padding/inset`, рамки и радиусы,
  `font-size`, `box-shadow`, `gap`, `flex-grow`, `filter`, `backdrop-filter`, `clip-path`, `transform`, `transform-origin`, градиентный фон. Свойства, влияющие на
  раскладку (и `opacity`/`color`, запечённые в команды), заставляют пересчитывать раскладку на кадре.
- События `transitionend`, `animationstart/iteration/end` уходят в JS (`propertyName`, `animationName`, `elapsedTime`).
- **`:hover`, `:active`, `:focus`, `:focus-within`**: состояние хранится в `style.c`; при смене пересчитывается стиль только если страница использует такие правила.
- **Grid** (`l2_grid.inc`): линии и `span`, именованные линии и области, `grid-auto-flow` (`row|column|dense`), `grid-auto-rows/columns`, `grid-template-rows`,
  `minmax()/fit-content()/repeat(auto-fill|auto-fit)`, `justify-/align-items|self`, `justify-/align-content`, авто-поля.

### Стекинг, прокрутка внутри элементов, sticky

- **`z-index`**: контексты (позиционированные блоки и flex-элементы с `z-index`, `position:fixed`, корень) записывают диапазон
  команд display list; после раскладки `z<0` переносится в начало содержимого охватывающего контекста, `z>0` — в конец
  (`stk_reorder` в `layout2.c`). Вокруг перенесённого диапазона восстанавливаются `C_CLIP/C_UNCLIP`, поэтому отсечение и прокрутка сохраняются.
  Ограничение: `fixed`-команды рисуются отдельным проходом поверх обычных независимо от `z-index`.
- **`overflow:auto|scroll`** (по осям): `C_CLIP` несёт `flags` (`CF_SCROLL_X/Y`), `node` и `ex[]` (размеры содержимого, максимум прокрутки).
  Смещения хранятся по `idx` узла (`l2_scroll_*`), переживают перерасчёт раскладки. Колесо идёт в самый вложенный контейнер под курсором
  (`webpage_scroll_at`), на границе событие уходит наружу. Оверлейные полосы рисуются при закрытии контейнера. В column-flex элементы
  с overflow сжимаются по `flex-shrink`, в row-flex растянутые элементы ограничиваются высотой строки — иначе прокрутки не возникает.
- **`position:sticky`** (`top`/`bottom`): блок обёрнут в `C_STK/C_ENDSTK`; смещение считается при отрисовке относительно ближайшего
  прокручиваемого предка (или окна) и ограничено content-box родителя (для контейнера-скроллера — всей прокручиваемой областью).
  `left`/`right` работают так же: `C_STK` несёт `SF_LEFT/SF_RIGHT`, `ex[4..7]` (смещения и границы контейнера по X), `Frame` — `rx0/rx1`.
- Единый автомат `Frame` (`paint.c`) используется для рисования, hit-test, выделения и колеса — все видят страницу одинаково.
- JS: `scrollTop/scrollLeft/scrollHeight/scrollWidth/clientHeight/clientWidth`, `scrollTo/scrollBy/scrollIntoView` у элементов-контейнеров.

## JavaScript

Ядро языка — **QuickJS** (`mods/frame/quickjs/`, MIT; ES2023: классы, `async/await`, генераторы, промисы, RegExp, Map/Set, BigInt, модули).
Браузерная часть:

| файл | что делает |
|---|---|
| `js.c` | нативные DOM-примитивы (`__dom.*`): дерево с id узлов, мутации + перелинеаризация, атрибуты, `innerHTML`, селекторы (тот же движок, что в CSS), геометрия из layout, таймеры, `fetch` (синхронно), ES-модули; жизненный цикл страницы |
| `js_prelude.js` → `js_prelude.c` | весь веб-API поверх примитивов: `Node/Element/Document`, события (capture/bubble, `on*`-атрибуты), `classList/style/dataset`, `location/history/navigator`, `Storage`, `URL`, `fetch/XMLHttpRequest/Headers/Response`, `Intersection/Resize/MutationObserver`, `console`… После правок: `python3 tools/embed_js.py` |

Как это работает: дочерний процесс загрузки скачивает внешние `<script src>` и таблицы стилей и кладёт их в `cache/tab_N.bundle`;
родитель создаёт JS-контекст, исполняет скрипты по порядку, генерирует `DOMContentLoaded`/`load`. После мутаций DOM каскад пересчитывается
(без сети) и страница перерисовывается. `main.c` ставит `webpage_js_tick_async()` в очередь каждый кадр (таймеры, промисы) и неблокирующе опрашивает ответы. Безопасные неотменяемые события (`input`/`change`/focus/scroll и animation events) используют ограниченную очередь; их изменения объединяются в следующий DOM-снимок. `click`/`keydown`/`beforeinput`/`submit` остаются синхронными там, где браузеру нужен немедленный результат `preventDefault()`. Навигация, title, scroll и focus собираются в ответе `tick`, чтобы не выполнять отдельные блокирующие запросы на каждом кадре. Большие DOM-снимки всё ещё передаются одним ответом; потоковая/чанковая передача пока не реализована. Навигация из JS (`location.href`, `history.back()`) также доставляется через mailbox.

`MutationObserver` получает записи из нативных DOM-операций (атрибуты, текст и изменения дерева) и доставляет пакет асинхронно. `IntersectionObserver` вычисляет геометрию цели и корня, учитывает `rootMargin`/threshold и перепроверяет её таймером. `localStorage` хранится в SQLite по профилю и origin. `indexedDB` поддерживает базовые object stores, запросы, индексы и курсоры; метаданные и JSON-совместимые записи также сохраняются в SQLite. Service Worker регистрируется по безопасному origin, переживает перезапуск и может обрабатывать запросы `fetch()`; CacheStorage и ответы кеша сохраняются в той же базе.

Ограничения: IndexedDB пока не реализует полный structured clone (например, Blob/циклические объекты) и атомарный rollback транзакций; индексы и курсоры имеют ограниченную совместимость. Service Worker — облегчённая реализация: нет перехвата навигации основного документа и полного жизненного цикла/Clients API, а `Request`/`Response` не поддерживают потоковые тела. `IntersectionObserver` использует периодическую проверку геометрии, а не интеграцию с композитором. Эти API пока не гарантируют совместимость со всеми современными сайтами.

### fetch / XMLHttpRequest

Асинхронные (запрос идёт в отдельном процессе, JS-цикл и отрисовка не блокируются), любые методы, политика SOP/CORS — в нативном коде (`ncors.c`).
- **Тела — байты.** `Blob`/`File`/`Request`/`Response` хранят `Uint8Array`; тело запроса между JS и C передаётся как `ArrayBuffer` без потерь на `\0` и не-UTF-8. На Linux `fetch()`/XHR используют `D.fetchStreamStart`, `D.fetchStreamHeaders`, `D.fetchStreamRead` и `D.fetchStreamClose`: заголовки ответа доступны до EOF, а тело поступает по ограниченному OS pipe с backpressure.
  Принимаются строка, `URLSearchParams`, `FormData`, `Blob`, `ArrayBuffer`, любые TypedArray/`DataView` (с учётом `byteOffset`).
- **`Content-Type` по умолчанию** (если не задан): строка → `text/plain;charset=UTF-8`, `URLSearchParams` → `application/x-www-form-urlencoded;charset=UTF-8`,
  `FormData` → `multipart/form-data; boundary=…`, `Blob` → `blob.type`.
- **`FormData`**: `new FormData(form[, submitter])` собирает поля по HTML-правилам (checkbox/radio только отмеченные, `select`/`select[multiple]`, `textarea`, `file`, disabled и безымянные пропускаются);
  `append/set(name, blob, filename)` оборачивает `Blob` в `File`. Кодирование multipart: имена/файлы экранируются (`%22 %0D %0A`), значения с CRLF. `Response/Request.formData()` разбирает urlencoded и multipart.
- **Ответ**: `arrayBuffer/blob/bytes/text/json/formData`, повторное чтение → `TypeError`; `text()` — UTF-8 (BOM снимается); `statusText` — из строки статуса; одноимённые заголовки склеиваются через `, `.
- **Заголовки** проверяются (`TypeError` на недопустимое имя и на `\r \n \0` в значении) — вставка заголовков через `\n` невозможна.
- **`Request`**: `new Request(request, init)` объединяет; `clone()`; `GET/HEAD` с телом — `TypeError`. `AbortSignal.timeout()/any()` работают.
- **Text encoding and streams**: `TextEncoder.encodeInto()` не режет многобайтовую кодовую точку; `TextDecoder` поддерживает UTF-8, Windows-1252 и UTF-16LE/BE, `fatal`, BOM и неполные символы между чанками. Полный реестр кодировок WHATWG ещё отсутствует. `TextDecoderStream` и `TransformStream` передают ошибки в обе стороны; BYOB поддерживает обычные typed arrays/DataView, но `autoAllocateChunkSize` и detached buffers не реализованы полностью.
- `data:` и `blob:` (из `URL.createObjectURL`) URL читаются без сети.
- **XHR**: `readyState` 2→3→4, `responseType` `''|text|json|arraybuffer|blob`, `upload`-события (`loadstart/progress/load/loadend`, по факту отправки целиком), `timeout`, `abort`.
- Ограничения: на Linux `Response.body` является pull-driven byte stream, `Response.clone()` делит его через `tee()`, а XHR выдаёт `readystatechange`/`progress` по мере получения тела; отмена закрывает pipe и дочерний сетевой процесс. На Windows (`nal_win32`) потоковый интерфейс пока лишь выдаёт чанки из уже полностью загруженного WinHTTP-ответа; полноценная CORS-политика там не реализована. `redirect: 'manual'|'error'` поддерживается на Linux: manual возвращает `opaqueredirect` без статуса/заголовков/тела, error отклоняет промис на редиректе; для follow задаются корректные `Response.url`, `Response.redirected` и `XMLHttpRequest.responseURL`. Синхронный XHR на Linux поддерживает только `responseType` `''`/`text` и `timeout = 0`; иначе `send()` выбрасывает `InvalidAccessError`. На Windows синхронные cross-origin-запросы блокируются, пока не реализован CORS preflight. Кодировки текста ограничены набором, описанным в главном README.
- Проверка: `bash tests/net/fetch.sh` (нужны `xvfb-run` и собранный `./my_browser`; 49 сценариев, включая redirect follow/manual/error, Response.url и Response.redirected; multipart разбирается сервером независимо).
Пример: `./my_browser examples/js-demo/index.html`.

### SVG (`svg.c`)

Собственный растеризатор (подмножество SVG 1.1), подключён к общему конвейеру картинок (`images.c`), поэтому работают:
встроенный `<svg>`, `<img src="*.svg">`, `background-image:url(*.svg)`, а также `data:`-URI (SVG и обычные картинки, base64 и %-кодирование).

- **Фигуры и контуры:** `path` (все команды, дуги), `rect` (rx/ry), `circle`, `ellipse`, `line`, `polyline`, `polygon`; `g`, `a`, `switch`,
  `use`/`symbol`/`defs`, вложенный `svg` с `viewBox` и `preserveAspectRatio`; `transform` (matrix/translate/scale/rotate/skew).
- **Закраска:** цвета (имена, `#rgb[a]`, `rgb()/rgba()/hsl()`), `currentColor`, `linearGradient`/`radialGradient` (units, transform, spread, href-наследование),
  `fill-rule`, `opacity`/`fill-opacity`/`stroke-opacity`. **Обводка:** ширина, `linecap`, `linejoin`, `miterlimit`, `dasharray`/`dashoffset`.
- **Стили:** атрибуты представления, `style="..."`, `<style>` с простыми селекторами (тег, `.класс`, `#id`, составные, списки).
- **Антиалиасинг:** накопление знаковой площади (как в font-rs); вектор перерисовывается под нужный размер, а не растягивается.
- **Встроенный `<svg>`:** парсер HTML выбрасывает его содержимое, поэтому `html.c` собирает разметку и сохраняет в `cache/svg_<хэш>.svg`
  (путь — в `n->src`); узел становится заменяемым элементом-картинкой. `currentColor` берётся из CSS `color` элемента при отрисовке.
  Спрайты: svg с `<symbol>` сохраняются как `cache/sprite_<хэш origin>_*.svg`, и `<use href="#id">` из других svg страницы их находит.
- **Текст:** `text`/`tspan` через встроенный TTF-рендерер, с `font-family`, `font-size`, `font-weight`, `font-style`, `text-anchor` и `currentColor`/заливками.
- **Эффекты SVG:** `clipPath` (в т.ч. `objectBoundingBox`), `mask` (luminance/alpha, content/region units), а также фильтры
  `feGaussianBlur`, `feOffset`, `feColorMatrix`, `feFlood`, `feBlend`, `feComposite`, `feMerge` и `feDropShadow`.
  Операции фильтра выполняются над изолированным RGBA-слоем; неподдерживаемый примитив фильтра пропускается с сохранением входа.
- **Ограничения:** SVG `image`, `pattern`, `marker`, `textPath`, анимации и полный набор filter primitives ещё не реализованы;
  CSS-правила страницы (`.icon path{fill:red}`) внутрь встроенного svg не проникают, содержимое svg статично (DOM-изменения из JS не перерисовывают).
- Проверка: `python3 tests/svg/check.py` (22 сверки пикселей); фаззинг под ASAN/UBSan прошёл 4500 входов, включая патологические.


### Форматы и responsive images

`stb_image` обрабатывает PNG/JPEG/GIF/BMP. При наличии `pkg-config`-пакетов `libwebp` и/или `libavif` сборка включает соответствующие декодеры; без них проект по-прежнему собирается, а неподдерживаемые современные форматы не выбираются в `<picture>` при указанном `type`. `srcset` выбирает ближайший подходящий кандидат для `w`/`x`-дескрипторов, `sizes` понимает базовые длины `px`, `vw`, `vh`, `em`/`rem` и media-условия `min/max-width`, `min/max-height`, `orientation`.

Запуск регрессионных проверок декодеров и выбора источника: `make test-images`.


## Streaming media and experimental MSE (v70)

`media_byte_queue.c` provides a bounded SPSC byte queue. `media_start_stream()` connects that queue to non-blocking `pipe:0` inputs for the FFmpeg audio-PCM and video decoders (or `ffplay` only as an audio fallback). `media_poll()` pumps at most 256 KiB per poll and preserves partial writes independently for audio and video readers; queue closure becomes input EOF, and queue failure propagates as a backend failure after already accepted bytes are delivered. In the normal SDL path, FFmpeg emits signed 16-bit stereo PCM into a bounded 64 KiB SPSC ring consumed by an SDL audio callback. The producer keeps waiting PCM below about 1/3 second; the callback fills any unavailable tail with silence and records underrun sample-frames. To avoid advancing the media clock merely when bytes are copied into a callback buffer, the clock credits the previous callback's PCM block on the next callback invocation. `MediaBackendStatus` exposes credited PCM sample-frames, observed underrun frames, and waiting ring bytes for diagnostics. This is a better software playback clock than querying SDL's queued-byte count, but it is still not a hardware DAC timestamp and includes at least callback/device latency. On underrun, the media clock advances only for real PCM frames and therefore pauses while silence is emitted. The `__dom.mediaStreamStart/Append/End` bridge is used by the experimental `MediaSource` implementation.

The public subset exposes `MediaSource`, `SourceBuffer`, `URL.createObjectURL(MediaSource)`, `sourceopen`/`sourceended`, `addSourceBuffer()`, `appendBuffer()`, asynchronous `updatestart`/`update`/`updateend`, `abort()` before parsing starts, and `endOfStream()`. It accepts fragmented MP4 initialization boxes (`ftyp` + `moov`) and media fragments (`moof` + `mdat`). Box headers and payloads may be split across `appendBuffer()` calls, and init/media boxes can also be supplied in one call. Incomplete box data is retained under a 64 MiB parser limit; queued input is limited to 64 MiB on the JS side plus the native 8 MiB ring buffer. The native decoder starts lazily on `play()` and EOF is signalled after queued chunks are transferred. `currentTime` remains stationary until decoded data arrives, preventing startup stalls from consuming the full playback duration before the first frame.

This is not standards-complete MSE. Only one `SourceBuffer` is supported. `remove()` asynchronously removes complete append-aligned fMP4 media append items before decoder startup and rebuilds `buffered`; partial-time removal, post-start removal and eviction are not supported. It rejects a range if it would require deleting only part of an append or if a media fragment shares its append with initialization data. Sequence mode, non-zero `timestampOffset`, finite append windows, live seekable ranges, random-access seeking, reinitialization, and zero-sized BMFF boxes are unsupported and fail explicitly. Aborting is supported only before the append parser commits state; later aborts throw `NotSupportedError` because the decoder input is irreversible. `buffered` is inferred from `tfdt`/`trun` timestamps and track intersection, rather than native decoder buffer occupancy; consequently it is a submitted-sample timeline approximation. MIME codec support is a syntactic whitelist and still depends on installed FFmpeg codec support. The video output path now asks FFmpeg's `showinfo` filter for each frame's `pts_time` over a dedicated metadata pipe. `media_poll()` holds a complete RGBA frame until its presentation deadline on `CLOCK_MONOTONIC`; metadata is bounded and must correspond one-to-one with output frames. This avoids the previous `-re` input-rate heuristic and makes an 8 fps one-second fixture present over approximately 0.9–1.0 seconds. Frames with missing timestamps use a documented 30 fps fallback; an overflow in the bounded timestamp queue fails playback rather than silently desynchronizing frames. The v73 playback path uses an `atempo` chain in the FFmpeg PCM decoder and schedules video PTS against PCM progress reported by that backend's SDL callback. `MediaBackendStatus.current_time` reports the same clock and the JS media-element tick anchors `currentTime` to it; when a native audio clock is active, the getter no longer extrapolates from wall time between polls (particularly important during underrun). Pause, volume/mute changes, and playback-rate changes snapshot the latest native clock before restarting the backend. The status includes `audioLatencyEstimate`, a conservative lower-bound estimate equal to one negotiated SDL callback period; the scheduler compensates this period but does not pretend it is a hardware DAC timestamp. Video-only files use the monotonic PTS scheduler, and `ffplay` remains a fallback if SDL cannot open an output device. Actual host/device buffering latency and the `ffplay` fallback remain unsynchronized. Audio underruns and the bounded ring are covered by `make test-media-audio-underrun` in addition to `make test-media-av-clock`. When the SDL PCM master clock is active, decoded video frames more than a bounded PTS lateness threshold behind audio are dropped rather than presented stale; `MediaBackendStatus.video_frames_dropped` exposes this diagnostic, and `make test-media-av-late-frame-drop` induces a polling stall to verify recovery. Tests: `make test-media-av-clock`, `make test-media-stream-backend`, `make test-media-mse`, plus media and browser API regression targets.


### Playback rate (v71)

`HTMLMediaElement.playbackRate` accepts rates from 0.0625 through 4.0. Video scheduling divides PTS deltas by the rate; audio applies an equivalent chained FFmpeg `atempo` filter. A rate change during playback restarts the media pipeline at the current time; the MSE path replays retained fMP4 fragments and suppresses synthetic seek events for this internal restart. Rate tests cover video deadlines and actual PCM/SDL audio duration. Audio and video now share an approximate PCM-consumption clock, but codec startup offsets, hardware buffering, gapless rate changes, and guaranteed pitch preservation are not solved.


Internal restarts invoked with `__startBackend(false)` preserve an already-fired `playing` lifecycle and the corresponding `HAVE_FUTURE_DATA` ready state after data has loaded. This prevents an active `currentTime` seek, rate change, loop, or `ffplay` volume fallback from spuriously redispatching `canplay`/`playing`. An explicit `play()` after `pause()` still re-arms that lifecycle. `make test-media-restart-events` exercises seek and rate restarts, stationary paused time, and pause/resume event counts. The FFmpeg decode process is still restarted, so this is not gapless playback.

For ordinary file-backed media, an active `currentTime` seek now keeps `seeking` true until the restarted decoder reports usable output (a decoded video frame, or a running audio-only backend). `timeupdate` and `seeked` are emitted when that readiness boundary is reached, not immediately after the decoder process is spawned. If `pause()` interrupts a pending seek, the backend is frozen at the requested input position and the pending seek is settled before the pause lifecycle finishes; this avoids a permanently stuck `seeking` flag. Paused seeks that do not restart a backend remain synchronous in this implementation. `make test-media-seek-events` covers both active completion and pause-during-seek. The MSE experimental path keeps its separate synchronous indexed replay semantics.
