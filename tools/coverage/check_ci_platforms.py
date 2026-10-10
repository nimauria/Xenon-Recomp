#!/usr/bin/env python3
"""Fail overall CI unless both platform workflows passed for this event and SHA."""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.parse
import urllib.error
import urllib.request

WORKFLOWS = {"windows.yml": "windows", "linux.yml": "linux"}


def get_json(path: str, token: str) -> dict:
    request = urllib.request.Request(
        "https://api.github.com" + path,
        headers={"Accept": "application/vnd.github+json",
                 "Authorization": "Bearer " + token,
                 "X-GitHub-Api-Version": "2022-11-28"})
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response)


def workflow_result(repository: str, workflow: str, job_name: str,
                    sha: str, event: str, branch: str, token: str) -> str:
    query = urllib.parse.urlencode({"head_sha": sha, "event": event,
                                    "branch": branch, "per_page": 20})
    base = f"/repos/{repository}/actions"
    try:
        runs = get_json(f"{base}/workflows/{workflow}/runs?{query}", token).get("workflow_runs", [])
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            return "pending: workflow has not appeared in Actions yet"
        raise
    runs = [run for run in runs if run.get("head_sha") == sha and
            run.get("event") == event and run.get("head_branch") == branch]
    if not runs:
        return "pending: no same-commit run"
    run = max(runs, key=lambda row: row["id"])
    if run.get("status") != "completed":
        return f"pending: {run.get('status')}"
    if run.get("conclusion") != "success":
        return f"failed: workflow concluded {run.get('conclusion')} ({run.get('html_url')})"
    jobs = get_json(f"{base}/runs/{run['id']}/jobs?per_page=100", token).get("jobs", [])
    matching = [job for job in jobs if job.get("name") == job_name]
    if len(matching) != 1 or matching[0].get("conclusion") != "success":
        return f"failed: required job {job_name} was missing, skipped, or unsuccessful"
    return "success"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wait-seconds", type=int, default=0)
    args = parser.parse_args()
    repository = os.environ["GITHUB_REPOSITORY"]
    token = os.environ["GITHUB_TOKEN"]
    sha = os.environ["XENON_CI_SHA"]
    event = os.environ["XENON_CI_EVENT"]
    branch = os.environ["XENON_CI_BRANCH"]
    if event not in {"push", "pull_request", "workflow_dispatch"}:
        parser.error(f"unsupported event: {event}")
    deadline = time.monotonic() + args.wait_seconds
    while True:
        try:
            results = {workflow: workflow_result(repository, workflow, job, sha, event, branch, token)
                       for workflow, job in WORKFLOWS.items()}
        except (OSError, ValueError, KeyError) as exc:
            print(f"Could not inspect platform workflow results: {exc}", file=sys.stderr)
            return 1
        for workflow, result in results.items():
            print(f"{workflow} {sha[:12]}: {result}", flush=True)
        if any(result.startswith("failed:") for result in results.values()):
            return 1
        if all(result == "success" for result in results.values()):
            return 0
        if time.monotonic() >= deadline:
            print("Platform workflows did not both complete successfully before the deadline", file=sys.stderr)
            return 1
        time.sleep(min(30, max(1, deadline - time.monotonic())))


if __name__ == "__main__":
    raise SystemExit(main())
