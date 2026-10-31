# MLBrowser local script extensions (limited MVP)

This directory supports **local, explicitly enabled user scripts**. It is not a Chrome/Firefox WebExtensions implementation: there is no isolated world, browser extension API, background worker, permission prompt model, or store. Scripts run in the target page's JavaScript context after page initialization and therefore have the same access as page script. Only install scripts you trust.

## Layout

Create one directory per extension. IDs may contain ASCII letters, digits, `_` and `-` only.

```text
extensions/
  example-helper/
    manifest.txt
    content.js
```

`manifest.txt` is UTF-8, one `key=value` pair per line:

```ini
name=Example helper
version=1.0
match=https://example.com/*
match=https://docs.example.org/*
script=content.js
```

Multiple `match=` entries are combined. Wildcards use a simple case-insensitive `*`/`?` glob against the complete URL. `script=` must be a single file name in the extension directory; path separators and `..` are rejected. Scripts over 256 KiB are ignored. The manager is at `browser://extensions`.

Enable/disable state is saved in the local browser profile. Scripts are disabled in private mode and when the profile database is disabled. Script updates are loaded on the next page navigation; refresh the page to re-run a script.

## Safety and limitations

Treat local scripts as executable code. They run with the page's JavaScript privileges, do not run in an isolated execution world and can read or modify page DOM and page-visible state. This MVP deliberately has no extension marketplace and no remote auto-update. There is no Chrome/Firefox API compatibility, declarative permissions, content-script `run_at`, background page, service worker, or per-extension sandbox yet.
