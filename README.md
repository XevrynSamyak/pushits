pushit
Build + push a single file to GitHub in one command.

Works for C and Python. You save your file in VS Code, run pushit, and it compiles (so broken code never gets pushed), commits, and uploads to your GitHub repo.

VS Code → edit your-file.c
   ↓
pushit "my message"
   ↓
compiles → commits → pushes to GitHub
   ↓
Done
Install (one time)
You need git, python3, and the GitHub CLI (gh).

macOS:

brew install gh git
Linux:

sudo apt install gh  # or follow the GitHub CLI docs for your distro
Then:

# 1. Log in to GitHub (follow the prompts / browser)
gh auth login

# 2. Make git use GitHub's credentials for pushing
gh auth setup-git

# 3. Install pushit
git clone https://github.com/XevrynSamyak/pushit /tmp/pushit-install
mkdir -p ~/.local/bin
cp /tmp/pushit-install/pushit ~/.local/bin/pushit
chmod +x ~/.local/bin/pushit
Usage
pushit "my commit message"     # push the file you most recently changed
pushit -f myfile.c "msg"       # push a specific file
pushit --all -m "msg"          # push every changed file at once
pushit --no-build              # skip compile
pushit --no-run                # don't run the program / show its output
pushit --public                # create the GitHub repo as public (default: private)
Run it inside a folder and pushit will:

Pick the file you last edited (or use -f to choose one).
Build it — C files compile in isolation, Python files get a syntax check. If it fails, nothing is pushed.
Commit only that file.
Create a private GitHub repo named after the folder the first time, then push to it every time after.
For a "100 Days of Code" setup:

mkdir my-codes && cd my-codes     # open this folder in VS Code
pushit "day 1 done"               # writes your Day file + your message
How it works
A single ~290-line Python script — no dependencies. It calls git and gh under the hood. View the source in pushit.
