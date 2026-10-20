# Installing on Windows — step by step, for beginners

This is an offline tool: it searches for a "pretty" Telegram Wallet address (for
example one ending in `TG`). It installs nothing into the system and never talks
to the network.

## Which file to download

The [Releases](../../releases/latest) page has several files. You need **one** —
the one with "windows" in its name:

| File | What it is for |
|---|---|
| **`wallet-tg-windows-x86_64.zip`** | **Windows — this is the one** |
| `wallet-tg-linux-x86_64.tar.gz` | Linux (ordinary PC or server) |
| `wallet-tg-linux-aarch64.tar.gz` | Linux on ARM (Raspberry Pi) |
| `app-debug.apk` | Android phone |

Direct link to the right file:
<https://github.com/tdk000/wallet_tg_miner/releases/latest/download/wallet-tg-windows-x86_64.zip>

---

## Step 1. Download the archive

Click the link above (or open Releases and pick the file with "windows") — a
download of `wallet-tg-windows-x86_64.zip` starts, about 180 KB.

## Step 2. Unpack it

Open "Downloads", right-click the archive → "Extract All…" → "Extract". You get a
folder with two files:

```
wallet_tg_miner.exe
start.bat
```

You can move the folder wherever you like, for example to the Desktop.

## Step 3. Run it

**Double-click `wallet_tg_miner.exe`** — a wizard opens with a menu.
(`start.bat` is a simpler menu without the language choice.)

> **Windows may say "Windows protected your PC"**
> That happens with any program that has no paid code-signing certificate. Click
> **"More info"** → **"Run anyway"**. The tool installs nothing, asks for no
> administrator rights and makes no network requests.

## Step 4. Answer the wizard

Answer with digits, press Enter after each answer:

1. Language → `2` (English) or `1` (Russian)
2. What to do → `1` (find a pretty address)
3. Desired address ending → for example `TG` or `ton` (letters and digits, 2–4 characters)
4. Mode → `1` (working mode — **recommended**: this is the mode Telegram will show
   after you import the phrase)

Then progress starts, and at the end you get the address and 12 words.

---

## How long a search takes

| Ending | Example | Expected time |
|---|---|---|
| 2 characters | `UQ......TG` | seconds |
| 3 characters | `UQ.....ton` | 5–15 minutes |
| 4 characters | `UQ....wo1f` | 6–10 hours (leave it overnight) |
| 5 characters | — | weeks, better not to start |

While it runs the window can be **minimised** but not closed. To stop: **Ctrl + C**.
Powering the machine off restarts the search from scratch.

---

## What to do with the result

```
=== FOUND ===
  address     : UQ....TG
  mnemonic    : word1 word2 word3 ... word12

  RESULT   : YES - import this phrase into Telegram Wallet
```

1. **Save the 12 words** — they are the key to the money (on paper, not in a chat).
2. Telegram Wallet (@wallet) → "Import wallet" → enter those 12 words.
3. **Compare the address** in the wallet with the found one, character by character.
4. Only after that, fund the wallet.

If it says `RESULT : NO`, "quick" mode was selected (option 2 instead of 1). Such an
address will not appear in Telegram: run again and pick mode **1**.

---

## Troubleshooting

**"Windows protected your PC" / "App blocked"**
"More info" → "Run anyway". An antivirus may complain about an unsigned exe — add
the folder to exclusions if you trust the source (the file is built from this
repository's sources automatically, in GitHub Actions).

**There is no `wallet_tg_miner.exe` in the archive**
You downloaded the wrong file. You need `wallet-tg-windows-x86_64.zip`
(not `.tar.gz`, not `.apk`).

**The window closes immediately**
Start `wallet_tg_miner.exe` by double-clicking it inside the extracted folder, not
from the archive viewer (there the file is not really unpacked). Or run `start.bat`.

**Nothing seems to happen**
That is expected — the search is running. Progress (attempts, speed, percentage) is
printed in the same window. A 4-character ending takes hours.

**Uninstalling**
Delete the folder. Nothing was installed into the system, the registry is untouched.

---

## Important (short)

- The 12 words are access to the money. **Never** show them to anyone and never paste
  them anywhere except the import dialog of Telegram Wallet itself.
- Always **compare** the address Telegram shows after import with the found one.
- The tool makes no network requests: you can disconnect the network after start.
- No GPU is needed on Windows: by default everything runs on the CPU (all cores).
