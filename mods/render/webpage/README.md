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
(без сети) и страница перерисовывается. `main.c` вызывает `webpage_js_tick()` каждый кадр (таймеры, промисы), доставляет `click`/`keydown`/`input`/`submit`,
исполняет навигацию, запрошенную из JS (`location.href`, `history.back()`).

`MutationObserver` получает записи из нативных DOM-операций (атрибуты, текст и изменения дерева) и доставляет пакет асинхронно. `IntersectionObserver` вычисляет геометрию цели и корня, учитывает `rootMargin`/threshold и перепроверяет её таймером. `localStorage` хранится в SQLite по профилю и origin. `indexedDB` поддерживает базовые object stores, запросы, индексы и курсоры; метаданные и JSON-совместимые записи также сохраняются в SQLite. Service Worker регистрируется по безопасному origin, переживает перезапуск и может обрабатывать запросы `fetch()`; CacheStorage и ответы кеша сохраняются в той же базе.

Ограничения: IndexedDB пока не реализует полный structured clone (например, Blob/циклические объекты) и атомарный rollback транзакций; индексы и курсоры имеют ограниченную совместимость. Service Worker — облегчённая реализация: нет перехвата навигации основного документа и полного жизненного цикла/Clients API, а `Request`/`Response` не поддерживают потоковые тела. `IntersectionObserver` использует периодическую проверку геометрии, а не интеграцию с композитором. Эти API пока не гарантируют совместимость со всеми современными сайтами.

### fetch / XMLHttpRequest

Асинхронные (запрос идёт в отдельном процессе, JS-цикл и отрисовка не блокируются), любые методы, политика SOP/CORS — в нативном коде (`ncors.c`).
- **Тела — байты.** `Blob`/`File`/`Request`/`Response` хранят `Uint8Array`; между JS и C тело — `ArrayBuffer` (`D.fetchStart`/`D.fetchPoll`), без потерь на `\0` и не-UTF-8.
  Принимаются строка, `URLSearchParams`, `FormData`, `Blob`, `ArrayBuffer`, любые TypedArray/`DataView` (с учётом `byteOffset`).
- **`Content-Type` по умолчанию** (если не задан): строка → `text/plain;charset=UTF-8`, `URLSearchParams` → `application/x-www-form-urlencoded;charset=UTF-8`,
  `FormData` → `multipart/form-data; boundary=…`, `Blob` → `blob.type`.
- **`FormData`**: `new FormData(form[, submitter])` собирает поля по HTML-правилам (checkbox/radio только отмеченные, `select`/`select[multiple]`, `textarea`, `file`, disabled и безымянные пропускаются);
  `append/set(name, blob, filename)` оборачивает `Blob` в `File`. Кодирование multipart: имена/файлы экранируются (`%22 %0D %0A`), значения с CRLF. `Response/Request.formData()` разбирает urlencoded и multipart.
- **Ответ**: `arrayBuffer/blob/bytes/text/json/formData`, повторное чтение → `TypeError`; `text()` — UTF-8 (BOM снимается); `statusText` — из строки статуса; одноимённые заголовки склеиваются через `, `.
- **Заголовки** проверяются (`TypeError` на недопустимое имя и на `\r \n \0` в значении) — вставка заголовков через `\n` невозможна.
- **`Request`**: `new Request(request, init)` объединяет; `clone()`; `GET/HEAD` с телом — `TypeError`. `AbortSignal.timeout()/any()` работают.
- `data:` и `blob:` (из `URL.createObjectURL`) URL читаются без сети.
- **XHR**: `readyState` 2→3→4, `responseType` `''|text|json|arraybuffer|blob`, `upload`-события (`loadstart/progress/load/loadend`, по факту отправки целиком), `timeout`, `abort`.
- Не поддержано: потоковое чтение тела (`Response.body`), `redirect: 'manual'|'error'`, реальный прогресс загрузки/скачивания, кодировки ответа кроме UTF-8, синхронный XHR; на Windows (`nal_win32`) асинхронный путь по-прежнему синхронный и только GET.
- Проверка: `bash tests/net/fetch.sh` (нужны `xvfb-run` и собранный `./my_browser`; 33 сценария, multipart на стороне сервера разбирается независимо).
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
