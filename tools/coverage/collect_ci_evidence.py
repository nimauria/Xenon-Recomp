#!/usr/bin/env python3
"""Collect existing Windows/Linux CTest results for a branch revision.

No build or test is run here. The script reads the platform workflow runs that
already exist for the commit: the `ctest-results-<platform>` JUnit artifact
when the run uploaded one, otherwise the per-test lines in the job log. A
platform whose results cannot be read is recorded as unavailable with the
reason; it is never recorded as passing.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import sys
import time
import zipfile
from pathlib import Path

import evidence
from github_api import GitHub, GitHubError

PLATFORM_RUNS = {
    "linux": {"workflow": "linux.yml", "job": "linux", "artifact": "ctest-results-linux"},
    "windows": {"workflow": "windows.yml", "job": "windows", "artifact": "ctest-results-windows"},
}


def platform_runs(api: GitHub, workflow: str, branch: str, sha: str | None) -> list[dict]:
    query = {"branch": branch, "event": "push", "per_page": 30}
    if sha:
        query["head_sha"] = sha
    runs = api.get(f"/repos/{{repo}}/actions/workflows/{workflow}/runs", **query).get("workflow_runs", [])
    return sorted((run for run in runs if run.get("head_branch") == branch and (not sha or run.get("head_sha") == sha)),
                  key=lambda run: run["id"], reverse=True)


def select_runs(api: GitHub, branch: str, sha: str, wait_seconds: int,
                sleep=time.sleep, clock=time.monotonic) -> dict[str, tuple[dict | None, str]]:
    """Pick one run per platform: same commit if it completes in time, else the
    newest completed run on the branch (recorded with its own commit)."""
    deadline = clock() + max(0, wait_seconds)
    while True:
        chosen: dict[str, tuple[dict | None, str]] = {}
        pending = []
        for platform, spec in PLATFORM_RUNS.items():
            runs = platform_runs(api, spec["workflow"], branch, sha)
            completed = [run for run in runs if run.get("status") == "completed"]
            if completed:
                chosen[platform] = (completed[0], "same commit")
            elif runs:
                pending.append(f"{platform} run {runs[0]['id']} is {runs[0].get('status')}")
                chosen[platform] = (None, "pending")
            else:
                chosen[platform] = (None, "no run for this commit")
        if not pending or clock() >= deadline:
            break
        print("waiting for same-commit platform runs: " + "; ".join(pending), flush=True)
        sleep(min(60, max(1, deadline - clock())))
    for platform, (run, reason) in list(chosen.items()):
        if run is None:
            older = [candidate for candidate in platform_runs(api, PLATFORM_RUNS[platform]["workflow"], branch, None)
                     if candidate.get("status") == "completed"]
            chosen[platform] = (older[0], f"{reason}; using newest completed branch run") if older else (None, reason)
    return chosen


def read_tests(api: GitHub, run: dict, spec: dict) -> tuple[dict[str, str], str, str]:
    """Return (tests, source, note) for one platform run."""
    artifacts = api.get(f"/repos/{{repo}}/actions/runs/{run['id']}/artifacts", per_page=100).get("artifacts", [])
    for artifact in artifacts:
        if artifact.get("name") == spec["artifact"] and not artifact.get("expired"):
            try:
                archive = zipfile.ZipFile(io.BytesIO(api.download(
                    f"/repos/{{repo}}/actions/artifacts/{artifact['id']}/zip")))
                for name in sorted(archive.namelist()):
                    if name.endswith(".xml"):
                        return evidence.parse_junit(archive.read(name).decode("utf-8")), "junit", ""
            except (GitHubError, zipfile.BadZipFile, ValueError, KeyError) as exc:
                note = f"JUnit artifact unreadable ({exc}); "
                break
    else:
        note = ""
    jobs = api.get(f"/repos/{{repo}}/actions/runs/{run['id']}/jobs", per_page=100).get("jobs", [])
    matching = [job for job in jobs if job.get("name") == spec["job"]]
    if len(matching) != 1:
        return {}, "log", note + f"run has no single job named {spec['job']}"
    log = api.download(f"/repos/{{repo}}/actions/jobs/{matching[0]['id']}/logs").decode("utf-8", "replace")
    tests = evidence.parse_ctest_log(log)
    return tests, "log", note + ("" if tests else "job log contains no CTest result lines")


def collect(api: GitHub, branch: str, sha: str, wait_seconds: int = 0, sleep=time.sleep,
            clock=time.monotonic) -> dict:
    result = {"schema": 1, "repository": api.repository, "branch": branch, "requested_commit": sha,
              "platforms": {}}
    try:
        chosen = select_runs(api, branch, sha, wait_seconds, sleep, clock)
    except GitHubError as exc:
        for platform in PLATFORM_RUNS:
            result["platforms"][platform] = {"unavailable": f"cannot list workflow runs: {exc}"}
        return result
    for platform, (run, reason) in chosen.items():
        spec = PLATFORM_RUNS[platform]
        if run is None:
            result["platforms"][platform] = {"unavailable": reason}
            continue
        record = {"workflow": spec["workflow"], "job": spec["job"], "run_id": run["id"],
                  "url": run.get("html_url"), "commit": run.get("head_sha"),
                  "conclusion": run.get("conclusion"), "selection": reason}
        try:
            tests, source_kind, note = read_tests(api, run, spec)
        except GitHubError as exc:
            tests, source_kind, note = {}, "log", f"cannot read results: {exc}"
        record["source"] = source_kind
        if tests:
            record["tests"] = dict(sorted(tests.items()))
        else:
            record["unavailable"] = note or "no CTest results"
        if note and tests:
            record["note"] = note
        result["platforms"][platform] = record
    return result


def summarize(data: dict) -> str:
    lines = []
    for platform, record in sorted(data["platforms"].items()):
        if "tests" in record:
            counts: dict[str, int] = {}
            for outcome in record["tests"].values():
                counts[outcome] = counts.get(outcome, 0) + 1
            lines.append(f"{platform}: run {record['run_id']} at {record['commit'][:12]} ({record['conclusion']}, "
                         f"{record['source']}, {record['selection']}): "
                         + ", ".join(f"{count} {outcome}" for outcome, count in sorted(counts.items())))
        else:
            lines.append(f"{platform}: unavailable - {record.get('unavailable')}")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY"))
    parser.add_argument("--branch", required=True)
    parser.add_argument("--sha", required=True)
    parser.add_argument("--wait-minutes", type=int, default=0)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if not args.repository:
        parser.error("--repository or GITHUB_REPOSITORY is required")
    if not token:
        data = {"schema": 1, "repository": args.repository, "branch": args.branch, "requested_commit": args.sha,
                "platforms": {platform: {"unavailable": "no GITHUB_TOKEN available"} for platform in PLATFORM_RUNS}}
    else:
        data = collect(GitHub(args.repository, token), args.branch, args.sha, args.wait_minutes * 60)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(data, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    print(summarize(data))
    return 0


if __name__ == "__main__":
    sys.exit(main())
