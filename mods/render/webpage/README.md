# webpage — движок веб-страниц

Конвейер: **HTML → дерево → каскад CSS → раскладка → display list → отрисовка**.
Старый плоский движок (`parse_html` + `layout`) остаётся для интерфейса браузера и служебных страниц;
`layout_and_render()` сам переключается на этот модуль, если корень DOM создан `parse_document()` (`node->v2`).

| файл | что делает |
|---|---|
| `html.c` | токенайзер и построение дерева: raw-text (`style/script/textarea/title/noscript`), сущности, неявные закрытия тегов, кодировки (UTF-8, windows-1251, KOI8-R, latin1), URL-ы |
| `css.c` | цвета, длины и `calc()/min()/max()/clamp()`, `@media`, `@import`, `@font-face` (+`unicode-range`), декларации, селекторы (комбинаторы, `[attr]`, `:nth-child`, `:not/:is/:where`, …) |
| `style.c` | UA-стили, индекс правил, каскад (специфичность, `!important`), `var()`, наследование → `node->st` |
| `layout2.c` (+`l2_flex/grid/table.inc`) | блоки с схлопыванием отступов, строчный поток, float, inline-block, flex, grid (базовый), таблицы, relative/absolute/fixed |
| `paint.c` | скруглённые рамки с AA, тени, фоновые картинки, текст, формы, hit-test, выделение, GET-отправка форм |
| `images.c` | параллельная предзагрузка (в дочернем процессе загрузки), декодирование `stb_image` (PNG/JPEG/GIF/BMP), кэш |

Запуск с URL: `./my_browser https://pypi.org/` или `./my_browser /path/to/page.html`.

Не поддерживается: JavaScript-управляемая вёрстка, анимации, `transition`. (Шрифты: WOFF/WOFF2/CFF — см. `../ttf_render/README.md`;
псевдоэлементы `::before/::after` и `var()` работают; `font-weight` учитывается по начертаниям веб-шрифта.)

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
  Горизонтальный sticky (`left/right`) не поддерживается.
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

Ограничения: `fetch/XHR` — только GET и синхронный (блокирует UI на время запроса), нет Web Workers/WebSocket/`Intl`/canvas,
`localStorage` не переживает переход на другую страницу, `MutationObserver` — заглушка, `IntersectionObserver` срабатывает сразу.
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
- **Не поддерживается:** `text`, `image`, `filter`, `mask`, `clip-path`, `pattern`, `marker`, анимации; CSS-правила страницы (`.icon path{fill:red}`)
  внутрь встроенного svg не проникают; содержимое svg статично (DOM-изменения из JS не перерисовывают).
- Проверка: `python3 tests/svg/check.py` (15 сверок пикселей); фаззинг под ASAN/UBSan прошёл 4500 входов, включая патологические.
