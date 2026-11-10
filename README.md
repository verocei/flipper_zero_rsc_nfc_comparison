# RSC NFC Comparison

Flipper Zero app that reads an NFC card, reads a card a second time, and checks
that the second read matches the first one **100%**: card type, UID and every
byte of data the card gives out without keys.

Typical uses: checking that a card reads back the same every time, checking a
copy against the original, or checking a batch of cards one after the other
against one reference card.

Built against the **Momentum** SDK (API 87.1). It uses only the shared OFW NFC
API, so it also builds for **stock (OFW)** firmware that has the same NFC
protocols (including NTAG4xx and Type 4 Tag).

The interface is available in **English** (default after install), **Hungarian**
and **German**. Pick it in the first row of **Settings**. That row is called
**Language** in every language, so it can be found even after picking one you
don't read.

The look follows the other RSC apps (RSC Remote Analyzer, RSC NFC Sequencer),
with the same "R" app icon.

> Magyar használati útmutató: [HASZNALAT.md](HASZNALAT.md).

## Build

```bash
# one-time: install the micro Flipper Build Tool
python3 -m pip install --upgrade ufbt

# in this project folder:
ufbt            # builds dist/rsc_nfc_comparison.fap
ufbt launch     # builds, uploads to a connected Flipper, and runs it
```

**Windows: no `!` in the project path.** ufbt rejects `!` in the compiler command
line (`MSLexError: Unquoted CMD metacharacters`). Build through a junction:

```powershell
New-Item -ItemType Junction -Path $HOME\flipper -Target "C:\!programozas\flipper_zero_programok"
cd $HOME\flipper\RSC-NFC-Comparison
ufbt
```

The `.fap` only runs on firmware with the same API version, so build once for
each target. It installs to `/ext/apps/NFC/`.

## Use

The main screen has the app name on top, a big **Start** button and smaller rows
below. **↑/↓** select, **OK** opens, **Back** exits.

1. **Start**: the **1st read** screen. Hold the card to the back of the Flipper.
   The tile reads *Waiting for card* while it looks for a card and turns solid
   (*Reading...*) while the card is being read. **OK** or **Back** cancels.
2. **1st read OK** shows the card type and UID. Press **→** (*2nd read*) when
   ready. You can leave the same card in place, or present a different card.
   With **Auto 2nd read** on (see Settings), this screen counts down
   (*2nd read in 3 s*) and the 2nd read starts by itself. **→** starts it at
   once, **←** / **Back** cancels.
3. **2nd read**: as in step 1. The 1st card's type and UID are shown below the
   tile.
4. **Result**:
   - **100% MATCH** (filled banner, green LED): everything that was read is the
     same.
   - **DIFFERENT** (framed banner, red LED): at least one byte differs. The rows
     show where:
     - **Type**: protocol and card type (e.g. NTAG215).
     - **UID**: the card's UID.
     - **Data**: everything else the card gave out. It is compared even when the
       UID differs, so you can tell "same content, other UID" apart from
       "different content". For Mifare Ultralight / NTAG it also says *what*
       differs: the number of pages, or *counter*, *signature*, *version*.
       *n/a* means the two cards are different types and can't be compared.
   - **↑/↓** switches to the second page: both reads (type + UID), and how many
     2nd reads matched the 1st one in this session.
   - **→ Next**: read another card against the **same** 1st read (for checking a
     batch of cards).
   - **OK New**: start over with a new 1st read.
   - **← Menu** / **Back**: back to the main screen.

After a 1st read, the main screen also shows **Compare with 1st**. It starts a
new 2nd read against the kept 1st read. The bottom line names that card.

A failed read (card moved away too soon) shows *Read failed* for a moment, and
the app tries again with whatever card is presented.

### Settings

- **Language** (this label in every language): English (default) · Magyar ·
  Deutsch.
- **Auto 2nd read**: Off (default) · 1 s · 2 s · 3 s · 5 s. When on, the 2nd
  read starts by itself after the 1st read, once this time has passed on the
  *1st read OK* screen. The pause leaves time to swap cards, so the 1st card is
  not simply read again at once. It only applies after a 1st read. **→ Next** on
  the result screen still starts the next card by hand.
- **LED**: Off / On. Cyan blink while reading, then green (match) or red
  (different) for as long as the result is shown.
- **Vibro**: Off / On. One buzz on a good read or a match, two on a mismatch.
- **Beep**: Off / On. A short beep on the 1st read, two high beeps on a match,
  a long low tone on a mismatch.
- **Version**: the app version (read-only).

Settings are saved to `/ext/apps_data/rsc_nfc_comparison/settings.bin`.

## What is compared

The app uses the firmware's own NFC scanner and protocol readers, the same read
the built-in NFC app does, and compares the two results field by field
(`nfc_device_is_equal`). It **never tries keys or passwords**.

| Card | What is read and compared |
| --- | --- |
| Mifare Ultralight, NTAG21x | all pages readable without a password, version, signature, counters, tearing flags |
| ISO15693 / SLIX (ICODE) | UID, system info, blocks readable without a password |
| DESFire, Mifare Plus, NTAG4xx, Type 4 Tag | what the card discloses without authentication |
| FeliCa, ST25TB, ISO14443-3A/3B/4A/4B | the openly readable data |
| **Mifare Classic** | **UID, ATQA, SAK only.** Sector data needs keys. The type is marked with `*` |
| Bank cards (EMV) | ISO14443-4A level only (UID, ATQA, SAK, ATS) |

Notes:

- Cards that use a random UID on every read (phones, some bank and DESFire
  cards) always show **UID: DIFF**. That result is correct.
- NTAG21x chips with the NFC counter enabled count up on every read. Two reads
  of the same card then differ in the counter (**Data: DIFF (counter)**). That
  result is correct too.

## License

[MIT](LICENSE) © 2026 Athos / RSC
