#!/usr/bin/env python3
"""Coverage-refresh orchestration for .github/workflows/coverage-refresh.yml.

Subcommands:
  target   resolve and check which branch revision a workflow event inspects
  run      regenerate the dashboard and reports, compare them with the
           committed snapshot, and write the proposal (read-only credentials)
  propose  publish a proposal produced by `run` as a bot-managed pull request

`propose` only ever commits the generated files listed in ALLOWED_PATHS, never
force-pushes, and leaves the bot branch alone if anyone else has pushed to it.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import audit
import evidence
import generate

TARGET_BRANCH = "development-restructure"
BOT_BRANCH = "bot/coverage-refresh"
BOT_NAME = "github-actions[bot]"
BOT_EMAIL = "41898282+github-actions[bot]@users.noreply.github.com"
MARKER = "<!-- xenon-coverage-refresh -->"
PR_TITLE = "Refresh generated coverage dashboard"
ALLOWED_PATHS = tuple(str(path.relative_to(generate.ROOT)).replace("\\", "/") for path in generate.GENERATED)
BODY_LIMIT = 60000


class RefreshError(RuntimeError):
    pass


# --- target resolution -------------------------------------------------------

def resolve_target(event: str, ref: str, configured: str = TARGET_BRANCH) -> dict:
    """Which revision a workflow event should inspect.

    Scheduled runs always execute the workflow file from the default branch,
    and manual runs may be started from any ref, so both inspect the configured
    development branch explicitly. A push is honoured only for that branch and
    pins the exact pushed commit.
    """
    if event == "push":
        if ref != f"refs/heads/{configured}":
            raise RefreshError(f"push to {ref} is not the coverage target refs/heads/{configured}")
        return {"branch": configured, "pin_event_sha": True}
    if event in {"schedule", "workflow_dispatch"}:
        return {"branch": configured, "pin_event_sha": False}
    raise RefreshError(f"coverage refresh does not run for {event} events")


def checkout_ref(event: str, ref: str, sha: str, configured: str = TARGET_BRANCH) -> str:
    target = resolve_target(event, ref, configured)
    return sha if target["pin_event_sha"] else f"refs/heads/{target['branch']}"


# --- run ---------------------------------------------------------------------

def prior_snapshot() -> dict | None:
    prior = generate.read_json(generate.INVENTORY_JSON)
    if prior is None:
        return None
    candidates = generate.read_json(generate.CANDIDATES_JSON) or {}
    merged = dict(prior)
    merged["candidates"] = candidates.get("candidates", [])
    return merged


def counts_table(before: dict | None, after: dict) -> list[str]:
    columns = ("listed", "classified", "verified", "implemented_unverified", "partial", "stub",
               "unimplemented", "unassessed")
    heads = ("Listed", "Classified", "Verified", "Implemented, unverified", "Partial", "Stub",
             "Unimplemented", "Unassessed")
    lines = ["| Area | " + " | ".join(heads) + " |", "| --- |" + " ---: |" * len(columns)]
    for category, title in audit.AREAS.items():
        row = after.get(category, {})
        old = (before or {}).get(category, {})
        cells = []
        for column in columns:
            value, previous = row.get(column, 0), old.get(column)
            cells.append(f"{previous} → **{value}**" if previous is not None and previous != value else str(value))
        lines.append(f"| {title} | " + " | ".join(cells) + " |")
    return lines


def pr_body(report: dict, metadata: dict, problems: list[dict], candidates: list[dict],
            changed_paths: list[str], branch: str) -> str:
    commit = metadata.get("assessed_commit") or "unknown"
    errors = [row for row in problems if row["severity"] == "error"]
    warnings = [row for row in problems if row["severity"] == "warning"]
    needs_review = (report["new_candidates"] or report["new_problems"] or report["lost_verification"]
                    or report["removed"] or report["renamed"] or report["stub_changes"])
    lines = [MARKER, "## Coverage dashboard refresh", "",
             f"Assessed `{branch}` at `{commit}`. CI evidence: {generate.describe_ci(metadata)}.", "",
             "This pull request was opened by the coverage-refresh workflow. It changes only generated files:", ""]
    lines += [f"- `{path}`" for path in changed_paths]
    lines += ["", "### Updated automatically",
              "- Listed counts from the source-declared inventories (kernel registrations, PPC catalog, Xenos shader forms, PM4 enums).",
              "- Displayed states from the reviewed `tools/coverage/coverage.json` entries, with reviewed `verified` entries shown as verified only while their required CTest target has a passing CI result.",
              "- The SVG dashboards, `REPORT.md`, `KERNEL_REFERENCE.md`, `inventory.json`, `audit-candidates.json` and `AUDIT.md`.",
              "", "### Needs human review",
              "- No classification was changed. Audit candidates are suggestions; promoting one requires a reviewed edit to `tools/coverage/coverage.json` under the [verification policy]("
              + f"https://github.com/{os.environ.get('GITHUB_REPOSITORY', 'nimauria/Xenon-Recomp')}/blob/{branch}/docs/coverage/VERIFICATION_POLICY.md).",
              f"- {len(candidates)} open audit candidates are listed in `docs/coverage/AUDIT.md`; {len(report['new_candidates'])} are new in this revision.",
              f"- Validation problems: {len(errors)} errors, {len(warnings)} warnings."
              + (" Review the findings below before merging." if needs_review else " Only generated assets changed."),
              "", "### Counts", ""]
    lines += counts_table((report["counts"] or {}).get("before"), report["counts"]["after"])
    if problems:
        lines += ["", "### Validation problems", ""]
        lines += [f"- **{row['severity']}** {row['kind']}" + (f" `{row['id']}`" if row.get("id") else "")
                  + f": {row['detail']}" for row in problems[:40]]
        if len(problems) > 40:
            lines.append(f"- … {len(problems) - 40} more in `docs/coverage/AUDIT.md`")
    lines += ["", "### Changes since the committed snapshot", "", audit.render_diff(report)]
    body = "\n".join(lines).rstrip() + "\n"
    if len(body) > BODY_LIMIT:
        body = body[:BODY_LIMIT] + "\n\n… truncated; see the workflow artifact `diff.json`.\n"
    return body


def write_outputs(values: dict[str, str]) -> None:
    path = os.environ.get("GITHUB_OUTPUT")
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            for key, value in values.items():
                handle.write(f"{key}={value}\n")


def append_summary(text: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(text + "\n")


def run(out_dir: Path, ci_evidence: Path | None, source_commit: str | None, branch: str,
        write: bool = True) -> dict:
    """Regenerate in place, compare with the committed files and write reports."""
    prior = prior_snapshot()
    data = evidence.read_evidence_file(ci_evidence) if ci_evidence else None
    model, metadata, outputs = generate.generate(generate.MANIFEST, data, source_commit, strict=False,
                                                 prior=generate.read_json(generate.INVENTORY_JSON))
    changed_paths = sorted(path.replace("\\", "/") for path in generate.stale_products(outputs))
    if write:
        for path, text in outputs.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8", newline="\n")
    current = dict(generate.snapshot(model), metadata=metadata)
    report = audit.diff(prior, current)
    problems = model.problems + generate.metadata_problems(metadata)
    errors = [row for row in problems if row["severity"] == "error"]
    status = {"changed": bool(changed_paths), "valid": not errors, "changed_paths": changed_paths,
              "assessed_commit": metadata.get("assessed_commit"), "errors": len(errors),
              "warnings": sum(row["severity"] == "warning" for row in problems),
              "candidates": len(model.candidates)}
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "diff.json").write_text(json.dumps(report, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    (out_dir / "diff.md").write_text(audit.render_diff(report), encoding="utf-8")
    (out_dir / "status.json").write_text(json.dumps(status, indent=1, sort_keys=True) + "\n", encoding="utf-8")
    (out_dir / "pr-body.md").write_text(pr_body(report, metadata, problems, model.candidates, changed_paths, branch),
                                        encoding="utf-8")
    summary = ["## Coverage refresh", "",
               f"Checked `{branch}` at `{source_commit or metadata.get('assessed_commit')}`; "
               f"files record `{metadata.get('assessed_commit')}` as the assessed revision.", "",
               *counts_table((report["counts"] or {}).get("before"), report["counts"]["after"]), "",
               f"Generated files changed: {', '.join(changed_paths) if changed_paths else 'none'}.",
               f"Validation: {status['errors']} errors, {status['warnings']} warnings; {status['candidates']} audit candidates.", ""]
    if errors:
        summary += ["### Errors (no proposal will be published)", ""]
        summary += [f"- {row['kind']}" + (f" `{row['id']}`" if row.get("id") else "") + f": {row['detail']}" for row in errors]
        summary.append("")
    summary += ["### Changes since the committed snapshot", "", audit.render_diff(report)]
    (out_dir / "summary.md").write_text("\n".join(summary), encoding="utf-8")
    append_summary("\n".join(summary))
    write_outputs({"changed": str(status["changed"]).lower(), "valid": str(status["valid"]).lower()})
    return status


def make_patch(git: "Git", out: Path) -> list[str]:
    """Write the generated-file changes as a binary patch.

    Fails if anything outside ALLOWED_PATHS changed, so a proposal can never
    carry runtime source, tests, the manifest or workflow edits.
    """
    status = git("status", "--porcelain", "--untracked-files=all", strip=False)
    changed = sorted({line[3:].split(" -> ")[-1].strip('"') for line in status.splitlines() if line.strip()})
    outside = [path for path in changed if path not in ALLOWED_PATHS]
    if outside:
        raise RefreshError("refresh changed files outside the generated coverage outputs: " + ", ".join(outside))
    if changed:
        git("add", "--intent-to-add", "--", *changed)
    patch = git("diff", "--binary", "--", *ALLOWED_PATHS, strip=False) if changed else ""
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(patch, encoding="utf-8", newline="\n")
    return changed


# --- propose -----------------------------------------------------------------

class Git:
    def __init__(self, cwd: Path, runner=subprocess.run):
        self.cwd = cwd
        self.runner = runner
        self.log: list[list[str]] = []

    def __call__(self, *args: str, check: bool = True, input_text: str | None = None, strip: bool = True) -> str:
        command = ["git", *args]
        if args and args[0] == "push" and any(arg in {"-f", "--force", "--force-with-lease"} or arg.startswith(("+", "--force"))
                                             for arg in args[1:]):
            raise RefreshError("refusing to force-push")
        self.log.append(command)
        env = dict(os.environ, GIT_AUTHOR_NAME=BOT_NAME, GIT_AUTHOR_EMAIL=BOT_EMAIL,
                   GIT_COMMITTER_NAME=BOT_NAME, GIT_COMMITTER_EMAIL=BOT_EMAIL)
        result = self.runner(command, cwd=self.cwd, capture_output=True, text=True, input=input_text, env=env)
        if check and result.returncode != 0:
            raise RefreshError(f"{' '.join(command)} failed: {result.stderr.strip() or result.stdout.strip()}")
        if result.returncode != 0:
            return ""
        return result.stdout.strip() if strip else result.stdout

    def ok(self, *args: str) -> bool:
        return self.runner(["git", *args], cwd=self.cwd, capture_output=True, text=True).returncode == 0


def patch_paths(git: Git, patch: Path) -> list[str]:
    output = git("apply", "--numstat", "-z", str(patch))
    return sorted({field.split("\t")[-1] for field in output.split("\0") if "\t" in field})


def generated_blobs(git: Git, rev: str) -> dict[str, str]:
    blobs = {}
    for path in ALLOWED_PATHS:
        blobs[path] = git("rev-parse", f"{rev}:{path}", check=False)
    return blobs


def foreign_commits(git: Git, bot_head: str, base_ref: str) -> list[str]:
    rows = git("log", "--format=%H %ae", bot_head, "--not", base_ref)
    return [line.split()[0] for line in rows.splitlines() if line and line.split()[1] != BOT_EMAIL]


def propose(git: Git, api, patch: Path | None, body: str, branch: str, target_sha: str,
            bot_branch: str = BOT_BRANCH, retire: bool = False) -> str:
    """Create or update the bot pull request. Returns a one-line outcome."""
    owner = api.repository.split("/")[0]
    git("fetch", "--no-tags", "origin", f"refs/heads/{branch}:refs/remotes/origin/{branch}")
    exists = bool(git("ls-remote", "--heads", "origin", f"refs/heads/{bot_branch}"))
    if exists:
        git("fetch", "--no-tags", "origin", f"refs/heads/{bot_branch}:refs/remotes/origin/{bot_branch}")
    bot_ref = f"refs/remotes/origin/{bot_branch}"
    pulls = api.get("/repos/{repo}/pulls", state="open", head=f"{owner}:{bot_branch}", base=branch)
    pull = pulls[0] if pulls else None
    foreign = foreign_commits(git, bot_ref, f"refs/remotes/origin/{branch}") if exists else []
    if foreign:
        return (f"left {bot_branch} untouched: it contains commits not made by the bot "
                f"({', '.join(sha[:12] for sha in foreign)})")
    if retire:
        if pull:
            api.post(f"/repos/{{repo}}/issues/{pull['number']}/comments",
                     {"body": f"{MARKER}\nClosing: `{branch}` at `{target_sha[:12]}` already contains up-to-date generated coverage files."})
            api.patch(f"/repos/{{repo}}/pulls/{pull['number']}", {"state": "closed"})
            return f"closed obsolete pull request #{pull['number']}"
        return "nothing to publish"
    if patch is None:
        raise RefreshError("a patch is required to publish a proposal")
    if exists:
        bot_base = git("merge-base", bot_ref, f"refs/remotes/origin/{branch}")
        if bot_base != target_sha and git.ok("merge-base", "--is-ancestor", target_sha, bot_base):
            return f"skipped: {bot_branch} already reflects a newer {branch} revision ({bot_base[:12]})"
    git("checkout", "--quiet", "--detach", target_sha)
    git("reset", "--quiet", "--hard", target_sha)
    paths = patch_paths(git, patch)
    outside = [path for path in paths if path not in ALLOWED_PATHS]
    if outside or not paths:
        raise RefreshError("proposal may only change generated coverage files; got " + ", ".join(outside or ["nothing"]))
    try:
        git("apply", "--index", str(patch))
        tree = git("write-tree")
    finally:
        git("reset", "--quiet", "--hard", target_sha)
    commit = None
    if exists and generated_blobs(git, bot_ref) == {path: git("rev-parse", f"{tree}:{path}", check=False)
                                                    for path in ALLOWED_PATHS}:
        outcome = f"{bot_branch} already has these generated files"
    else:
        parents = ["-p", bot_ref] if exists else []
        if not exists or not git.ok("merge-base", "--is-ancestor", target_sha, bot_ref):
            parents += ["-p", target_sha]
        message = f"Refresh generated coverage dashboard for {branch} {target_sha[:12]}\n"
        commit = git("commit-tree", tree, *parents, input_text=message)
        git("push", "origin", f"{commit}:refs/heads/{bot_branch}")
        outcome = f"pushed {commit[:12]} to {bot_branch}"
    if pull is None:
        created = api.post("/repos/{repo}/pulls", {"title": PR_TITLE, "head": bot_branch, "base": branch,
                                                    "body": body, "maintainer_can_modify": True})
        return outcome + f"; opened pull request #{created['number']}"
    if pull.get("body") != body or pull.get("title") != PR_TITLE:
        api.patch(f"/repos/{{repo}}/pulls/{pull['number']}", {"title": PR_TITLE, "body": body})
        return outcome + f"; updated pull request #{pull['number']}"
    return outcome + f"; pull request #{pull['number']} already current"


# --- command line ------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    target = commands.add_parser("target")
    target.add_argument("--event", required=True)
    target.add_argument("--ref", required=True)
    target.add_argument("--sha", required=True)
    target.add_argument("--head", help="checked-out commit; must equal --sha for push events")
    run_parser = commands.add_parser("run")
    run_parser.add_argument("--out-dir", type=Path, required=True)
    run_parser.add_argument("--ci-evidence", type=Path)
    run_parser.add_argument("--source-commit")
    run_parser.add_argument("--branch", default=TARGET_BRANCH)
    patch_parser = commands.add_parser("patch")
    patch_parser.add_argument("--out", type=Path, required=True)
    publish = commands.add_parser("propose")
    publish.add_argument("--patch", type=Path)
    publish.add_argument("--body", type=Path)
    publish.add_argument("--branch", default=TARGET_BRANCH)
    publish.add_argument("--target-sha", required=True)
    publish.add_argument("--bot-branch", default=BOT_BRANCH)
    publish.add_argument("--retire", action="store_true", help="close an obsolete bot pull request")
    args = parser.parse_args(argv)
    try:
        if args.command == "target":
            resolved = resolve_target(args.event, args.ref)
            if args.head and resolved["pin_event_sha"] and args.head != args.sha:
                raise RefreshError(f"checked out {args.head[:12]}, expected pushed commit {args.sha[:12]}")
            write_outputs({"branch": resolved["branch"]})
            print(f"coverage target: {resolved['branch']} "
                  f"({'event commit' if resolved['pin_event_sha'] else 'branch head'})")
            return 0
        if args.command == "run":
            status = run(args.out_dir, args.ci_evidence, args.source_commit, args.branch)
            print(json.dumps(status, indent=1, sort_keys=True))
            return 0 if status["valid"] else 1
        if args.command == "patch":
            changed = make_patch(Git(generate.ROOT), args.out)
            print("generated files changed: " + (", ".join(changed) if changed else "none"))
            return 0
        from github_api import GitHub
        token = os.environ.get("GITHUB_TOKEN")
        if not token or not os.environ.get("GITHUB_REPOSITORY"):
            raise RefreshError("GITHUB_TOKEN and GITHUB_REPOSITORY are required to publish")
        body = args.body.read_text(encoding="utf-8") if args.body else ""
        outcome = propose(Git(generate.ROOT), GitHub(os.environ["GITHUB_REPOSITORY"], token), args.patch, body,
                          args.branch, args.target_sha, args.bot_branch, args.retire)
        print(outcome)
        append_summary(f"Proposal: {outcome}")
        return 0
    except (RefreshError, generate.CoverageError, ValueError) as exc:
        print(f"coverage refresh error: {exc}", file=sys.stderr)
        append_summary(f"**Coverage refresh error:** {exc}")
        return 1
    except Exception as exc:  # GitHubError and unexpected failures
        print(f"coverage refresh error: {type(exc).__name__}: {exc}", file=sys.stderr)
        append_summary(f"**Coverage refresh error:** {type(exc).__name__}: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
