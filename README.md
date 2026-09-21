# pushit

Build + push your work to GitHub in one command.

Works for C and Python. You save your file in VS Code, run `pushit`, and it compiles
(so broken code never gets pushed), runs it so you can see the output, commits, and
uploads to your GitHub repo.

```
VS Code → edit your-file.c
   ↓
pushit "my message"
   ↓
compiles → runs → commits → pushes to GitHub
   ↓
Done
```

## Install (one time)

You need `git`, `python3`, and the GitHub CLI (`gh`).

macOS:

```bash
brew install gh git
```

Linux:

```bash
sudo apt install gh   # or follow the GitHub CLI docs for your distro
```

Then:

```bash
# 1. Log in to GitHub (follow the prompts / browser)
gh auth login

# 2. Make git use GitHub's credentials for pushing
gh auth setup-git

# 3. Install pushit
git clone https://github.com/XevrynSamyak/pushits /tmp/pushit-install
mkdir -p ~/.local/bin
cp /tmp/pushit-install/pushit.c ~/.local/bin/pushit
chmod +x ~/.local/bin/pushit
```

(The script is Python despite the `.c` name — that is just what the file is called in this repo.)

## Usage

```bash
pushit "my commit message"     # push the file you most recently changed
pushit -f myfile.c "msg"       # push a specific file (repeat -f for several)
pushit -i                      # pick files from a numbered list
pushit --all -m "msg"          # push every changed file at once
pushit -y -m "msg"             # quick mode: no prompts at all
pushit --dry-run               # show git status and change nothing
pushit --no-build              # skip compile
pushit --no-run                # don't run the program / show its output
pushit --public                # create the GitHub repo as public (default: private)
pushit --repo my-repo -m "msg"        # push to a repo under your account
pushit --repo owner/repo -m "msg"     # push to any existing repo, or create it if missing
pushit -b dev --ssh                   # branch and transport (remembered per project)
pushit --reset                        # forget this project's remembered settings
```

Run it inside a folder and pushit will:

1. Pick the file you last edited (or use `-f` to name one, or `-i` to choose from a list).
2. **Build it** — C files compile in isolation, Python files get a syntax check.
   If it fails, nothing is pushed.
3. **Run it** and show you the output (10s timeout, `--no-run` to skip).
4. Commit only the files you chose.
5. Create a private GitHub repo named after the folder the first time, then push to it
   every time after. Use `--repo` to push to or create a different repo.

For a "100 Days of Code" setup:

```bash
mkdir my-codes && cd my-codes     # open this folder in VS Code
pushit "day 1 done"               # builds, runs, commits and pushes your Day file
```

## Choosing files

`-i` lists everything git sees as changed or untracked and lets you pick:

```
Changed / untracked files:
    1. day7.c
    2. notes.md
    3. utils.py
Stage which? (a=all, n=none, e.g. 1,3,5-7): 1,3
```

Before committing you can ask to see `git diff --cached`; if it isn't what you meant,
answer `n` and pushit unstages everything and lets you choose again.

At the commit-message prompt, press Enter for the default, type a message, or type `e`
to write it in `$EDITOR`.

## Settings it remembers

| Where | What |
|---|---|
| `.pushconfig.json` (in your project) | repo, branch, https/ssh — so later runs need no flags |
| `~/.push_to_github_config.json` | your GitHub username |

`--reset` clears the project file. `.pushconfig.json` is added to `.gitignore`
automatically, so it never gets committed.

## When a push fails

pushit prints the actual next steps instead of a stack trace — the `git pull --rebase`
to run, the `--allow-unrelated-histories` variant, how to check your SSH key or PAT,
and what `origin` currently points at.

## How it works

A single ~560-line Python script — no dependencies. It calls `git` and `gh` under the
hood. `pushit --selftest` runs its built-in checks. View the source in
[pushit.c](pushit.c).
