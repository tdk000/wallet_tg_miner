# Installing on Ubuntu — step by step, for beginners

This is an offline tool: it searches for a "pretty" Telegram Wallet address
(for example one ending in `TG`). It runs only on your computer and never
talks to the network.

There are two ways. **Way 1 is easier** — nothing has to be compiled.

---

## Way 1. Prebuilt binary (recommended)

Only `curl` is needed and it ships with Ubuntu. No `sudo`, no build step.

### Step 1. Open a terminal

Press **Ctrl + Alt + T**.
If nothing happens: press the **Windows** (Super) key, type `Terminal`, launch it.

A dark window with a blinking cursor appears — that is expected.

### Step 2. Copy one line, paste it, press Enter

```
mkdir -p ~/wallet-tg && cd ~/wallet-tg && curl -L -o wallet-tg https://github.com/tdk000/wallet_tg_miner/releases/latest/download/wallet_tg_miner_linux_x86_64 && chmod +x wallet-tg && ./wallet-tg --selftest && echo "alias wallet-tg='$HOME/wallet-tg/wallet-tg --wizard'" >> ~/.bashrc && echo "READY"
```

**Pasting:** in a terminal it is **Ctrl + Shift + V** (not Ctrl + V), or
right-click → "Paste".

About 1.3 MB is downloaded and the built-in self-test runs immediately. Takes seconds.

### Step 3. Check the result

The end of the output must look like this:

```
[ok]   fallback check checksum error
all self-tests passed
READY
```

`all self-tests passed` means the download is intact and the cryptography matches
the reference vectors.

### Step 4. Close the terminal and open it again

Once, so the short launch command is remembered. Press **Ctrl + Alt + T** again,
type `wallet-tg`, press Enter. A menu opens.

### Step 5. Use the menu

Answer with digits, press Enter after each answer:

1. Language → `2` (English) or `1` (Russian)
2. What to do → `1` (find a pretty address)
3. Desired address ending → for example `TG` or `ton` (letters and digits, 2–4 characters)
4. Mode → `1` (working mode — **recommended**: this is the mode that produces an
   address Telegram will actually show after importing the phrase)

The tool prints progress, finds an address and shows it together with 12 words.

### On ARM (Raspberry Pi, ARM server)

Check with `uname -m`. If it prints `aarch64`, replace
`wallet_tg_miner_linux_x86_64` with `wallet_tg_miner_linux_aarch64` in the line
from step 2.

---

## Way 2. Build from source

If you want to compile it yourself or have an unusual system. `git` and a compiler
are installed by the line itself (it asks for your Ubuntu account password).

```
sudo apt update && sudo apt install -y g++ git && rm -rf ~/wallet_tg_miner && git clone -q https://github.com/tdk000/wallet_tg_miner ~/wallet_tg_miner && cd ~/wallet_tg_miner && sh build.sh && ./wallet_tg_miner --selftest && echo "alias wallet-tg='$HOME/wallet_tg_miner/wallet_tg_miner --wizard'" >> ~/.bashrc && echo "READY"
```

**About the password:** while you type it, **nothing is displayed** (not even
asterisks). That is normal — type it blind and press Enter.

**How long:** 1–3 minutes (the compiler download is about 100 MB).

Then follow way 1 from step 3 (check), step 4 (reopen the terminal), step 5 (menu).

---

## How long a search takes

The longer the ending, the longer the search. For an ordinary home computer (4 cores):

| Ending | Example | Expected time |
|---|---|---|
| 2 characters | `UQ......TG` | seconds |
| 3 characters | `UQ.....ton` | 5–15 minutes |
| 4 characters | `UQ....wo1f` | 6–10 hours (leave it overnight) |
| 5 characters | — | weeks, better not to start |

While it runs: the terminal window can be **minimised** but not closed. Powering the
machine off restarts the search from scratch. To stop: **Ctrl + C**.

---

## What to do with the result

The tool prints something like:

```
=== FOUND ===
  address     : UQ....TG
  mnemonic    : word1 word2 word3 ... word12

  RESULT   : YES - import this phrase into Telegram Wallet
```

1. **Save the 12 words** (they are the key to the money — on paper, not in a chat).
2. Telegram Wallet (@wallet) → "Import wallet" → enter those 12 words.
3. **Compare the address** in the wallet with the found one, character by character.
4. Only after that, fund the wallet.

If it says `RESULT : NO`, "quick" mode was selected (option 2 instead of 1). Such an
address will not appear in Telegram. Run `wallet-tg` again and pick mode **1**.

---

## Troubleshooting

**`wallet-tg: command not found`**
Close and reopen the terminal (step 4). Still no luck — run it by full path:
`~/wallet-tg/wallet-tg --wizard` (way 2: `~/wallet_tg_miner/wallet_tg_miner --wizard`).

**`curl: command not found`**
Install it: `sudo apt install -y curl`, then repeat step 2.

**Wrong password / password asked again**
This is your Ubuntu account password, not a Telegram one. It is not shown while typing —
type it blind and press Enter.

**`Unable to locate package g++`**
No internet, or stale package lists. Check the connection and repeat the whole line.

**`Permission denied`**
Way 1 already runs `chmod +x`; if you launch the file yourself, run `chmod +x wallet-tg`
in its directory first.

**Updating to a newer version**
Way 1: repeat the line from step 2 (the file is overwritten).
Way 2: repeat the line from way 2.

**Uninstalling**
Way 1: `rm -rf ~/wallet-tg && sed -i '/wallet-tg=/d' ~/.bashrc`
Way 2: `rm -rf ~/wallet_tg_miner && sed -i '/wallet-tg=/d' ~/.bashrc`

---

## Important (short)

- The 12 words are access to the money. **Never** show them to anyone and never paste
  them anywhere except the import dialog of Telegram Wallet itself. No "checking
  service" should ever ask for them.
- Always **compare** the address Telegram shows after import with the found one, and
  only then transfer funds.
- The tool makes no network requests: you can disconnect the network after installing
  and the search still works.
- The prebuilt static binaries cannot use the GPU. For `--gpu`, build dynamically
  (see README, section 2).
