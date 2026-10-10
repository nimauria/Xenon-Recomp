"""Minimal GitHub REST client for the coverage automation (standard library only)."""

from __future__ import annotations

import json
import urllib.error
import urllib.parse
import urllib.request


class GitHubError(RuntimeError):
    def __init__(self, message: str, status: int | None = None):
        super().__init__(message)
        self.status = status


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class GitHub:
    """Calls api.github.com with a token. Redirects to log/artifact storage are
    followed without the Authorization header, which storage hosts reject."""

    def __init__(self, repository: str, token: str, api: str = "https://api.github.com"):
        self.repository = repository
        self.token = token
        self.api = api.rstrip("/")
        self._opener = urllib.request.build_opener(_NoRedirect)

    def _request(self, method: str, url: str, body: dict | None = None, accept: str = "application/vnd.github+json"):
        data = None if body is None else json.dumps(body).encode("utf-8")
        request = urllib.request.Request(url, data=data, method=method, headers={
            "Accept": accept, "Authorization": "Bearer " + self.token,
            "X-GitHub-Api-Version": "2022-11-28", "User-Agent": "xenon-coverage-refresh",
            **({"Content-Type": "application/json"} if data is not None else {})})
        try:
            return self._opener.open(request, timeout=60)
        except urllib.error.HTTPError as exc:
            if exc.code in {301, 302, 303, 307, 308} and exc.headers.get("Location"):
                return urllib.request.urlopen(exc.headers["Location"], timeout=120)
            detail = exc.read().decode("utf-8", "replace")[:500]
            raise GitHubError(f"{method} {url} failed with HTTP {exc.code}: {detail}", exc.code) from None
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            raise GitHubError(f"{method} {url} failed: {exc}") from None

    def url(self, path: str, query: dict | None = None) -> str:
        path = path.replace("{repo}", self.repository)
        return self.api + path + ("?" + urllib.parse.urlencode(query) if query else "")

    def json(self, method: str, path: str, query: dict | None = None, body: dict | None = None):
        with self._request(method, self.url(path, query), body) as response:
            raw = response.read()
        return json.loads(raw) if raw else None

    def get(self, path: str, **query):
        return self.json("GET", path, query or None)

    def post(self, path: str, body: dict):
        return self.json("POST", path, body=body)

    def patch(self, path: str, body: dict):
        return self.json("PATCH", path, body=body)

    def download(self, path: str) -> bytes:
        with self._request("GET", self.url(path), accept="application/vnd.github+json") as response:
            return response.read()
