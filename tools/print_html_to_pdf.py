#!/usr/bin/env python3
"""Safe, source-preserving PDF printing for MLBrowser.

The main document is read from a browser-created temporary file. Network subresources
are restricted to the current document origin; data: resources are allowed; file:, ftp:,
javascript: and cross-origin fetches are rejected to avoid turning printing into a local-file
or arbitrary-network disclosure primitive.
"""
from __future__ import annotations
import os
import sys
import urllib.error
import urllib.request
from urllib.parse import urlsplit

try:
    from weasyprint import HTML
except Exception as exc:  # pragma: no cover - environment-dependent fallback in C
    HTML = None
    _IMPORT_ERROR = exc
else:
    _IMPORT_ERROR = None


def _origin(url: str):
    try:
        parts = urlsplit(url)
        scheme = parts.scheme.lower()
        if scheme not in ("http", "https") or not parts.hostname:
            return None
        port = parts.port or (443 if scheme == "https" else 80)
        return scheme, parts.hostname.rstrip(".").lower(), port
    except (ValueError, UnicodeError):
        return None


def is_allowed_resource_url(url: str, base_url: str) -> bool:
    """Whether a page subresource can be fetched during printing."""
    try:
        scheme = urlsplit(url).scheme.lower()
    except (ValueError, UnicodeError):
        return False
    if scheme == "data":
        return True
    if scheme not in ("http", "https"):
        return False
    base_origin = _origin(base_url)
    return base_origin is not None and _origin(url) == base_origin


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        # Redirects are intentionally rejected. Otherwise a same-origin URL could redirect
        # the fetcher to file:/ or another host after our origin check.
        return None


def make_fetcher(base_url: str):
    opener = urllib.request.build_opener(_NoRedirect())

    def fetch(url: str, timeout: int = 10, ssl_context=None):
        parts = urlsplit(url)
        if parts.scheme.lower() == "data":
            from weasyprint.urls import default_url_fetcher
            return default_url_fetcher(url, timeout=timeout, ssl_context=ssl_context)
        if not is_allowed_resource_url(url, base_url):
            raise ValueError("Blocked non-same-origin print resource")
        req = urllib.request.Request(url, headers={"User-Agent": "MLBrowser-Print/1.0"})
        try:
            with opener.open(req, timeout=timeout) as response:
                content = response.read(16 * 1024 * 1024 + 1)
                if len(content) > 16 * 1024 * 1024:
                    raise ValueError("Print resource exceeds 16 MiB limit")
                headers = response.headers
                return {
                    "string": content,
                    "mime_type": headers.get_content_type(),
                    "encoding": headers.get_content_charset(),
                    "redirected_url": url,
                    "filename": os.path.basename(parts.path) or "resource",
                }
        except urllib.error.HTTPError as exc:
            raise ValueError(f"Print resource returned HTTP {exc.code}") from exc

    return fetch


def render_pdf(input_html: str, output_pdf: str, base_url: str) -> None:
    if HTML is None:
        raise RuntimeError(f"WeasyPrint unavailable: {_IMPORT_ERROR}")
    HTML(filename=input_html, base_url=base_url, url_fetcher=make_fetcher(base_url)).write_pdf(output_pdf)


def main(argv: list[str]) -> int:
    if len(argv) != 4:
        print("usage: print_html_to_pdf.py INPUT.html OUTPUT.pdf BASE_URL", file=sys.stderr)
        return 2
    try:
        render_pdf(argv[1], argv[2], argv[3])
        return 0
    except Exception as exc:
        print(f"PDF render failed: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
