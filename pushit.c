#!/usr/bin/env python3
"""
pushit - build, then push your work to GitHub in one command.

  pushit "my message"        build + commit + push the most recently changed file
  pushit -m "message"        same, message from a flag
  pushit -f day3.c "msg"     push a specific file instead (repeat -f for several)
  pushit -i                  pick from a numbered list of changed files
  pushit --all -m "msg"      push ALL changed files at once
  pushit -y -m "msg"         quick mode: no prompts at all
  pushit --dry-run           show git status and change nothing
  pushit --no-build          skip the build step
  pushit --no-run            don't run the program / show its output
  pushit --public            create the GitHub repo as public (default: private)
  pushit --repo my-repo -m "msg"       push to a repo under your account
  pushit --repo owner/repo -m "msg"    push to any existing repo, or create it if missing
  pushit -b dev --ssh                  branch and transport (remembered per project)
  pushit --reset             forget this project's remembered settings

Repo, branch and https/ssh are remembered in .pushconfig.json next to your code,
and your GitHub username in ~/.push_to_github_config.json, so later runs need no flags.
"""

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

GITIGNORE = """\
# Python
__pycache__/
*.py[cod]
.venv/
venv/
env/
*.egg-info/
dist/
build/

# C / C++
*.o
*.a
*.so
*.exe
*.out

# OS / editors
.DS_Store
"""

PROJECT_BUILDS = (
    ("Makefile", "make"),
    ("CMakeLists.txt", "cmake -S . -B build && cmake --build build"),
)

PROJECT_CONFIG = ".pushconfig.json"
GLOBAL_CONFIG = Path.home() / ".push_to_github_config.json"

ROOT = Path.cwd()
QUICK = False  # -y: never prompt, take every default
sys.stdout.reconfigure(line_buffering=True)


def log(msg, tag=".."):
    print(f"[{tag}] {msg}")


def die(msg, r=None):
    """Exit 1. With a CompletedProcess, show why it failed first."""
    if r is not None and (r.stdout or r.stderr).strip():
        print((r.stdout + r.stderr).rstrip(), file=sys.stderr)
    sys.exit(f"[!] {msg}")


def ask(prompt, default=""):
    """Ask the user; empty input, no TTY or -y falls back to `default`."""
    if QUICK:
        return default
    try:
        ans = input(prompt).strip()
    except (EOFError, OSError, KeyboardInterrupt):
        return default
    return ans or default


def ask_yes(prompt, default=True):
    ans = ask(f"{prompt} [{'Y/n' if default else 'y/N'}]: ")
    return default if not ans else ans.lower().startswith("y")


def run(cmd):
    return subprocess.run(cmd, shell=True, text=True, capture_output=True, cwd=ROOT, errors="replace")


def git(args):
    return run("git " + args)


def q(p):
    return shlex.quote(str(p))


def load_json(path):
    try:
        data = json.loads(Path(path).read_text())
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def save_json(path, data):
    try:
        Path(path).write_text(json.dumps(data, indent=2) + "\n")
        return True
    except OSError as e:
        log(f"could not write {path}: {e}", "!")
        return False


def ensure_git(pref=None):
    """Make sure we're on a git repo and (if asked) on branch `pref`."""
    if not (ROOT / ".git").is_dir():
        r = git(f"init -b {q(pref or 'main')}")
        if r.returncode:
            die("could not initialize a git repo here", r)
        log("initialized git repo", "ok")
    cur = git("symbolic-ref --short HEAD").stdout.strip() or "main"
    if pref and pref != cur:
        r = git(f"checkout -B {q(pref)}")
        if r.returncode:
            die(f"could not switch to branch {pref}", r)
        log(f"on branch {pref}", "ok")
        cur = pref
    return cur


def ignore(*names):
    """Create .gitignore if missing, add `names`. True if it changed."""
    gi = ROOT / ".gitignore"
    created = not gi.exists()
    lines = GITIGNORE.splitlines() if created else gi.read_text(errors="ignore").splitlines()
    new = [n for n in names if n not in lines]
    if created:
        log("created .gitignore")
    if new:
        log(f"gitignored: {', '.join(new)}")
    if created or new:
        gi.write_text("\n".join(lines + new) + "\n")
    return created or bool(new)


def parse_status(text):
    """Paths out of `git status --porcelain -uall` output."""
    paths = []
    for line in text.splitlines():
        p = line[3:].strip().strip('"').split(" -> ")[-1]  # renames: 'R  old -> new'
        if p and not p.endswith("/"):
            paths.append(p)
    return paths


def changed_files():
    # -uall so files inside a brand-new directory are listed, not just the directory
    skip = (".gitignore", PROJECT_CONFIG)
    return [p for p in parse_status(git("status --porcelain -uall").stdout) if p not in skip]


def parse_selection(text, n):
    """'a' -> everything, 'n' -> nothing, '1,3 5-7' -> zero-based indices."""
    t = text.strip().lower()
    if t in ("a", "all", ""):
        return list(range(n))
    if t in ("n", "none"):
        return []
    picked = set()
    for part in t.replace(",", " ").split():
        bits = part.split("-")
        if len(bits) > 2 or not all(b.isdigit() for b in bits):
            raise ValueError(f"not a number: {part}")
        lo, hi = int(bits[0]), int(bits[-1])
        if lo < 1 or hi > n or lo > hi:
            raise ValueError(f"out of range: {part}")
        picked.update(range(lo - 1, hi))
    return sorted(picked)


def pick_files(changed):
    """Numbered picker over changed/untracked files. Returns chosen paths."""
    if not changed:
        return []
    print("\nChanged / untracked files:")
    for i, p in enumerate(changed, 1):
        print(f"  {i:>3}. {p}")
    while True:
        try:
            idx = parse_selection(ask("Stage which? (a=all, n=none, e.g. 1,3,5-7): ", "a"), len(changed))
        except ValueError as e:
            log(e, "!")
            continue
        return [changed[i] for i in idx]


def build(paths, project):
    """Compile / syntax-check `paths`. True if .gitignore changed."""
    for marker, cmd in (PROJECT_BUILDS if project else ()):
        if (ROOT / marker).exists():
            log(f"build: {cmd}")
            r = run(cmd)
            if r.returncode:
                die("build failed - fix the errors before pushing", r)
            log("build succeeded", "ok")
            return False

    srcs = [p for p in paths if p.suffix in (".c", ".cpp")]
    pys = [p for p in paths if p.suffix == ".py"]
    if not srcs and not pys:
        log("no build step for these files")
        return False

    touched = False
    if srcs:
        cc = shutil.which("gcc") or shutil.which("cc") or "cc"
        for src in srcs:
            out = src.with_suffix("")
            log(f"build: {cc} -Wall -Wextra -o {out.name} {src.name}")
            r = run(f"{cc} -Wall -Wextra -o {q(out)} {q(src)}")
            if r.returncode:
                die(f"compile failed for {src.name} - fix it before pushing", r)
            touched |= ignore(out.name)
        log(f"compiled {len(srcs)} source(s)", "ok")
    if pys:
        log(f"build: syntax-checking {len(pys)} python file(s)")
        r = run("python3 -m compileall -q " + " ".join(q(p) for p in pys))
        if r.returncode:
            die("python syntax check failed - fix it before pushing", r)
        log("python syntax check passed", "ok")
    return touched


def show_output(fp, timeout=10):
    if fp.suffix in (".c", ".cpp"):
        cmd, label = q(fp.with_suffix("")), fp.with_suffix("").name
    elif fp.suffix == ".py":
        cmd, label = "python3 " + q(fp), fp.name
    else:
        return
    try:
        r = subprocess.run(cmd, shell=True, text=True, capture_output=True,
                           stdin=subprocess.DEVNULL, timeout=timeout, cwd=ROOT, errors="replace")
    except subprocess.TimeoutExpired:
        log(f"{label} ran longer than {timeout}s - output skipped (--no-run to suppress)", "!")
        return
    print(f"--- output of {label} ---")
    print((r.stdout + r.stderr).rstrip() or "(no output)")
    print(f"(exit code {r.returncode})")


def stage(paths):
    if paths:
        git("add -A -- " + " ".join(q(p) for p in dict.fromkeys(str(p) for p in paths)))
    return [l for l in git("diff --cached --name-only").stdout.splitlines() if l]


def unstage():
    if git("rev-parse --verify -q HEAD").returncode == 0:
        git("reset -q")
    else:
        git("rm -r --cached -q -- .")


def compose_in_editor(default):
    """Write the commit message in $EDITOR (typed 'e' at the prompt)."""
    editor = (os.environ.get("GIT_EDITOR") or os.environ.get("VISUAL")
              or os.environ.get("EDITOR") or ("notepad" if os.name == "nt" else "vi"))
    fd, tmp = tempfile.mkstemp(prefix="pushit_msg_", suffix=".txt")
    os.close(fd)
    try:
        Path(tmp).write_text(default + "\n\n# Lines starting with '#' are ignored.\n")
        try:
            subprocess.call(shlex.split(editor, posix=(os.name != "nt")) + [tmp])
        except OSError as e:
            log(f"could not launch {editor!r}: {e}", "!")
            return default
        body = [l for l in Path(tmp).read_text().splitlines() if not l.startswith("#")]
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass
    return "\n".join(body).strip() or default


def commit(staged, message):
    default = f"auto-commit: {len(staged)} file(s)" if len(staged) != 1 else f"update {Path(staged[0]).name}"
    if not message:
        message = ask(f'Commit message ("{default}", or "e" for $EDITOR): ', default)
        if message.strip().lower() == "e":
            message = compose_in_editor(default)
    r = git(f"commit -m {q(message)}")
    if r.returncode:
        if "nothing to commit" in r.stdout:
            return log("nothing to commit")
        if "user.email" in (r.stdout + r.stderr):
            log('set your identity first: git config --global user.name "You"', "!")
            log('                         git config --global user.email "you@example.com"', "!")
        die("commit failed", r)
    log(f"committed: {message.splitlines()[0]}", "ok")


def gh_owner():
    """The logged-in GitHub username, cached in ~/.push_to_github_config.json."""
    cfg = load_json(GLOBAL_CONFIG)
    if cfg.get("username"):
        return cfg["username"]
    if run("gh auth status").returncode:
        die("not logged into GitHub - run  gh auth login  first, then retry")
    owner = run("gh api user --jq .login").stdout.strip()
    if not owner:
        die("could not determine your GitHub username")
    cfg["username"] = owner
    if save_json(GLOBAL_CONFIG, cfg):
        log(f"remembered your GitHub username in {GLOBAL_CONFIG}")
    return owner


def ensure_remote(public, repo_name=None, ssh=False):
    """Point origin at a GitHub repo, creating it if missing.

    With --repo, target that repo (creating it if it doesn't exist yet).
    Names without an "owner/" part are assumed to belong to the logged-in user.
    True if the repo was just created and already pushed.
    """
    if not repo_name:
        if "origin" in git("remote").stdout.split():
            return False
        repo_name = ROOT.name.strip().replace(" ", "-")

    if "/" not in repo_name:
        repo_name = f"{gh_owner()}/{repo_name}"

    field = "sshUrl" if ssh else "url"
    r = run(f"gh repo view {q(repo_name)} --json {field} --jq .{field}")
    if r.returncode == 0:
        url = r.stdout.strip()
        if "origin" in git("remote").stdout.split():
            git(f"remote set-url origin {q(url)}")
        else:
            git(f"remote add origin {q(url)}")
        log(f"targeting existing repo {repo_name}", "ok")
        return False

    vis = "--public" if public else "--private"
    log(f"creating GitHub repo '{repo_name}' ({vis[2:]})")
    r = run(f"gh repo create {q(repo_name)} {vis} --source . --remote origin --push")
    if r.returncode:
        die("could not create the repo on GitHub", r)
    if ssh:
        url = run(f"gh repo view {q(repo_name)} --json sshUrl --jq .sshUrl").stdout.strip()
        if url:
            git(f"remote set-url origin {q(url)}")
    log(f"pushed to {git('remote get-url origin').stdout.strip()}", "ok")
    return True


def push_help(branch, ssh):
    origin = git("remote get-url origin").stdout.strip()
    print("=" * 68, file=sys.stderr)
    print("PUSH FAILED - likely causes and fixes:", file=sys.stderr)
    print(f"  * remote has commits you don't:  git pull --rebase origin {branch}", file=sys.stderr)
    print(f"    (unrelated histories)          git pull --allow-unrelated-histories origin {branch}", file=sys.stderr)
    print(f"  * only if you're certain:        git push --force-with-lease origin {branch}", file=sys.stderr)
    if ssh:
        print("  * SSH key not registered:        ssh -T git@github.com", file=sys.stderr)
        print("    add one at https://github.com/settings/keys  (ssh-keygen -t ed25519)", file=sys.stderr)
    else:
        print("  * HTTPS auth: gh auth login && gh auth setup-git", file=sys.stderr)
        print("    or use a PAT as the password: https://github.com/settings/tokens (scope: repo)", file=sys.stderr)
    print(f"  * wrong target?  origin is {origin or '(unset)'}", file=sys.stderr)
    print("  * branch protected, or no write access on that repo", file=sys.stderr)
    print("=" * 68, file=sys.stderr)


def push(branch, ssh=False):
    for last in (False, True):
        r = git(f"push -u origin {q(branch)}")
        if not r.returncode:
            return log(f"pushed to origin/{branch}", "ok")
        if not last:
            log("push failed; retrying...", "!")
    print((r.stdout + r.stderr).rstrip(), file=sys.stderr)
    push_help(branch, ssh)
    sys.exit(1)


def selftest():
    global ROOT
    assert parse_status('?? a.c\n M b.py\nR  old.c -> new.c\n?? "sp ace.c"\n?? d/\n') == \
        ["a.c", "b.py", "new.c", "sp ace.c"]
    assert parse_status("") == []
    assert parse_selection("a", 3) == [0, 1, 2]
    assert parse_selection("", 3) == [0, 1, 2]
    assert parse_selection("N", 3) == []
    assert parse_selection("1,3", 3) == [0, 2]
    assert parse_selection("2-3 1", 3) == [0, 1, 2]
    assert parse_selection("2,2", 3) == [1]
    for bad in ("9", "0", "x", "3-1", "1-99", "1-2-3"):
        try:
            parse_selection(bad, 3)
        except ValueError:
            pass
        else:
            raise AssertionError(f"{bad!r} should have been rejected")
    with tempfile.TemporaryDirectory() as d:
        ROOT = Path(d)
        assert ignore() and (ROOT / ".gitignore").exists()
        assert ignore("day3") and "day3" in (ROOT / ".gitignore").read_text()
        assert not ignore("day3")
        cfg = ROOT / PROJECT_CONFIG
        assert save_json(cfg, {"repo": "r", "branch": "dev", "ssh": True})
        assert load_json(cfg) == {"repo": "r", "branch": "dev", "ssh": True}
        assert load_json(ROOT / "nope.json") == {}
    log("selftest passed", "ok")


def main():
    global ROOT, QUICK
    ap = argparse.ArgumentParser(
        description="Build + push your work to GitHub.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("message", nargs="?", help="commit message (otherwise prompted)")
    ap.add_argument("-m", "--message", dest="flag_message", help="commit message from a flag")
    ap.add_argument("-f", "--file", action="append",
                    help="file to push instead of the most recently changed one (repeatable)")
    ap.add_argument("-i", "--pick", action="store_true", help="choose files from a numbered list")
    ap.add_argument("--all", action="store_true", help="commit all changed files instead of one")
    ap.add_argument("-y", "--quick", action="store_true", help="no prompts at all; take every default")
    ap.add_argument("--dry-run", action="store_true", help="show git status and change nothing")
    ap.add_argument("--no-build", action="store_true", help="skip the build step")
    ap.add_argument("--no-run", action="store_true", help="skip running the program (10s timeout)")
    ap.add_argument("--public", action="store_true", help="create repo as public (default: private)")
    ap.add_argument("-r", "--repo", help="push to this GitHub repo (owner/repo, or a name under your account)")
    ap.add_argument("-b", "--branch", help="branch to push (remembered per project; default main)")
    ap.add_argument("--ssh", action="store_true", help="use the SSH remote URL")
    ap.add_argument("--https", action="store_true", help="use the HTTPS remote URL")
    ap.add_argument("--reset", action="store_true", help=f"forget this project's {PROJECT_CONFIG}")
    ap.add_argument("--selftest", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.selftest:
        return selftest()
    if args.ssh and args.https:
        die("--ssh and --https are mutually exclusive")
    QUICK = args.quick

    for d in (Path.cwd().resolve(), *Path.cwd().resolve().parents):
        if (d / ".git").is_dir():
            ROOT = d
            break
    log(f"root: {ROOT}")

    if args.dry_run:
        if not (ROOT / ".git").is_dir():
            log("not a git repo yet - pushit would run `git init` here")
        else:
            print(git("status").stdout.rstrip())
        log("--dry-run: nothing built, staged, committed or pushed")
        return

    # Remembered per-project settings: repo, branch, https/ssh.
    cfgpath = ROOT / PROJECT_CONFIG
    if args.reset and cfgpath.exists():
        cfgpath.unlink()
        log(f"cleared {PROJECT_CONFIG}")
    cfg = {} if args.reset else load_json(cfgpath)

    ssh = True if args.ssh else False if args.https else bool(cfg.get("ssh", False))
    branch_pref = args.branch or cfg.get("branch")
    branch = ensure_git(branch_pref)
    touched = ignore(PROJECT_CONFIG)

    # Repo: explicit --repo, else remembered, else ask when origin is missing.
    repo = args.repo or cfg.get("repo")
    if not repo and "origin" not in git("remote").stdout.split():
        repo = ask(f"GitHub repo to push to (owner/name, Enter = {ROOT.name}): ") or None

    newcfg = {"branch": branch, "ssh": ssh}
    if repo:
        newcfg["repo"] = repo
    if newcfg != {k: cfg.get(k) for k in newcfg}:
        if save_json(cfgpath, newcfg):
            log(f"remembered repo/branch/transport in {PROJECT_CONFIG}")

    # Code: explicit -f, the picker, --all, or the most recently changed file.
    if not args.all and not args.pick and not args.file and args.repo is not None:
        chosen = ask("Code file to push (Enter = most recently changed): ")
        if chosen:
            args.file = [chosen]

    if args.all:
        paths = [ROOT / p for p in changed_files()]
    elif args.pick and not QUICK:
        paths = [ROOT / p for p in pick_files(changed_files())]
    elif args.file:
        paths = []
        for f in args.file:
            fp = Path(f)
            fp = fp if fp.is_absolute() else ROOT / fp
            if not fp.exists():
                die(f"no such file: {fp}")
            paths.append(fp)
    else:
        changed = changed_files()
        if changed:
            newest = max(changed, key=lambda p: (ROOT / p).stat().st_mtime if (ROOT / p).exists() else 0)
            log(f"most recently changed: {newest}")
            paths = [ROOT / newest]
        else:
            paths = []

    if not paths:
        log("no changes to push")
    else:
        if not args.no_build:
            touched |= build(paths, project=args.all or len(paths) > 1)
        if len(paths) == 1 and not args.no_run:
            show_output(paths[0])
    if touched:
        paths.append(ROOT / ".gitignore")

    # Stage, optionally eyeball the diff, and back out / reselect if it's wrong.
    while True:
        staged = stage(paths)
        if not staged:
            log("nothing staged")
            break
        log(f"staged: {', '.join(staged)}")
        if not ask_yes("Show `git diff --cached` before committing?", False):
            break
        print(git("diff --cached").stdout.rstrip())
        if ask_yes("Commit this?"):
            break
        unstage()
        if not ask_yes("Pick different files?"):
            die("stopped before committing (nothing staged, nothing pushed)")
        paths = [ROOT / p for p in pick_files(changed_files())]
        if not paths:
            die("nothing selected")

    if staged:
        commit(staged, args.flag_message or args.message)
    if not ensure_remote(args.public, repo, ssh):
        push(branch, ssh)
    print("Done.")


if __name__ == "__main__":
    main()
