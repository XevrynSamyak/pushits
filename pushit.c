#!/usr/bin/env python3
"""
pushit - build, then push your work to GitHub in one command.

  pushit "my message"        build + commit + push the most recently changed file
  pushit -m "message"        same, message from a flag
  pushit -f day3.c "msg"     push a specific file instead
  pushit --all -m "msg"      push ALL changed files at once
  pushit --no-build          skip the build step
  pushit --no-run            don't run the program / show its output
  pushit --public            create the GitHub repo as public (default: private)
"""

import argparse
import shlex
import shutil
import subprocess
import sys
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

ROOT = Path.cwd()
sys.stdout.reconfigure(line_buffering=True)


def log(msg, tag=".."):
    print(f"[{tag}] {msg}")


def die(msg, r=None):
    """Exit 1. With a CompletedProcess, show why it failed first."""
    if r is not None and (r.stdout or r.stderr).strip():
        print((r.stdout + r.stderr).rstrip(), file=sys.stderr)
    sys.exit(f"[!] {msg}")


def run(cmd):
    return subprocess.run(cmd, shell=True, text=True, capture_output=True, cwd=ROOT)


def git(args):
    return run("git " + args)


def q(p):
    return shlex.quote(str(p))


def ensure_git():
    if not (ROOT / ".git").is_dir():
        r = git("init -b main")
        if r.returncode:
            die("could not initialize a git repo here", r)
        log("initialized git repo", "ok")
    return git("symbolic-ref --short HEAD").stdout.strip() or "main"


def ignore(*names):
    """Create .gitignore if missing, add `names`. True if it changed."""
    gi = ROOT / ".gitignore"
    created = not gi.exists()
    lines = GITIGNORE.splitlines() if created else gi.read_text(errors="ignore").splitlines()
    new = [n for n in names if n not in lines]
    if created:
        log("created .gitignore")
    if new:
        log(f"gitignored build output: {', '.join(new)}")
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
    return [p for p in parse_status(git("status --porcelain -uall").stdout) if p != ".gitignore"]


def pick_file(explicit):
    if explicit:
        fp = Path(explicit)
        fp = fp if fp.is_absolute() else ROOT / fp
        if not fp.exists():
            die(f"no such file: {fp}")
        return fp
    changed = changed_files()
    if not changed:
        return None
    fp = max(changed, key=lambda p: (ROOT / p).stat().st_mtime if (ROOT / p).exists() else 0)
    log(f"most recently changed: {fp}")
    return ROOT / fp


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
                           stdin=subprocess.DEVNULL, timeout=timeout, cwd=ROOT)
    except subprocess.TimeoutExpired:
        log(f"{label} ran longer than {timeout}s - output skipped (--no-run to suppress)", "!")
        return
    print(f"--- output of {label} ---")
    print((r.stdout + r.stderr).rstrip() or "(no output)")
    print(f"(exit code {r.returncode})")


def commit(paths, message):
    if not paths:
        return
    paths = list(dict.fromkeys(str(p) for p in paths))
    git("add -- " + " ".join(q(p) for p in paths))
    if not message:
        message = input("Commit message (or press Enter for auto): ").strip()
    message = message or f"auto-commit: {len(paths)} file(s)"
    r = git(f"commit -m {q(message)}")
    if r.returncode:
        if "nothing to commit" in r.stdout:
            return log("nothing to commit")
        die("commit failed", r)
    log(f"committed: {message}", "ok")


def ensure_remote(public):
    """Create origin on GitHub if there is none. True if it also pushed."""
    if "origin" in git("remote").stdout.split():
        return False
    if run("gh auth status").returncode:
        die("not logged into GitHub - run  gh auth login  first, then retry")
    name = ROOT.name.strip().replace(" ", "-")
    vis = "--public" if public else "--private"
    log(f"creating GitHub repo '{name}' ({vis[2:]})")
    r = run(f"gh repo create {q(name)} {vis} --source . --remote origin --push")
    if r.returncode:
        die("could not create the repo on GitHub", r)
    log(f"pushed to {git('remote get-url origin').stdout.strip()}", "ok")
    return True


def push(branch):
    for last in (False, True):
        r = git(f"push -u origin {branch}")
        if not r.returncode:
            return log(f"pushed to origin/{branch}", "ok")
        if not last:
            log("push failed; retrying...", "!")
    die("git push failed", r)


def selftest():
    import tempfile
    global ROOT
    assert parse_status('?? a.c\n M b.py\nR  old.c -> new.c\n?? "sp ace.c"\n?? d/\n') == \
        ["a.c", "b.py", "new.c", "sp ace.c"]
    assert parse_status("") == []
    with tempfile.TemporaryDirectory() as d:
        ROOT = Path(d)
        assert ignore() and (ROOT / ".gitignore").exists()
        assert ignore("day3") and "day3" in (ROOT / ".gitignore").read_text()
        assert not ignore("day3")
    log("selftest passed", "ok")


def main():
    global ROOT
    ap = argparse.ArgumentParser(
        description="Build + push your work to GitHub.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("message", nargs="?", help="commit message (otherwise prompted)")
    ap.add_argument("-m", "--message", dest="flag_message", help="commit message from a flag")
    ap.add_argument("-f", "--file", help="file to push instead of the most recently changed one")
    ap.add_argument("--all", action="store_true", help="commit all changed files instead of one")
    ap.add_argument("--no-build", action="store_true", help="skip the build step")
    ap.add_argument("--no-run", action="store_true", help="skip running the program (10s timeout)")
    ap.add_argument("--public", action="store_true", help="create repo as public (default: private)")
    ap.add_argument("--selftest", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    for d in (Path.cwd().resolve(), *Path.cwd().resolve().parents):
        if (d / ".git").is_dir():
            ROOT = d
            break
    log(f"root: {ROOT}")

    branch = ensure_git()
    touched = ignore()

    if args.all:
        paths = [ROOT / p for p in changed_files()]
    else:
        fp = pick_file(args.file)
        paths = [fp] if fp else []
    if not paths:
        log("no changes to push")
    else:
        if not args.no_build:
            touched |= build(paths, project=args.all)
        if not args.all and not args.no_run:
            show_output(paths[0])
    if touched:
        paths.append(ROOT / ".gitignore")

    commit(paths, args.flag_message or args.message)
    if not ensure_remote(args.public):
        push(branch)
    print("Done.")


if __name__ == "__main__":
    main()
