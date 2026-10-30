#!/usr/bin/env python3
"""Keep the announcer from naming players in a mod.

The commentary is built from sentences in
data/xenon/audio/speech/sentence/commentary/sentences.xml, each a list of
phrases, and a phrase picks its clip by tags. 23 of the 183 sentences say a
player's name: a phrase tagged `playerID` picks the clip recorded for that
player. A mod keeps the base game's commentary (no published mod changes any
audio; see tools/modkit.py) but replaces the players, so those clips name
someone who is not on the court, or the right surname for the wrong person.

So in a mod's root every sentence that names a player is rewritten without
the name:

  - Where the name is a phrase of its own ahead of the call - "Wade!" then
    the block - the name goes and the call stays: "...blocks it". A
    "with twelve points" after a name goes with it, since it says whose
    points they were.
  - Where the name is all the sentence has - "LeBron wins the tip" is one
    recording per player - the sentence is given the phrases of its
    nameless counterpart, "wins the tip", instead.

The sentence keeps its trigger name and id, which is how the game asks for
it, so nothing that chooses commentary has to change. The base game's own
root is never touched: there the names are right.

The file is written fresh rather than in place, because a mod's root is made
of hard links to the base game's files and writing through one would change
the base game too.

Usage:
    python tools/commentary.py <mod root>
"""

import os
import re
import sys
from pathlib import Path

SENTENCES = "data/xenon/audio/speech/sentence/commentary/sentences.xml"

# Written into the file so a second run can tell it has nothing to do.
MARK = "<!-- OpenJam: player names removed for a mod -->"

# For the sentences that are nothing but a name clip: which nameless sentence
# says the same thing.
NAMELESS = {
    "PASS_MADE_PS": "PASS_MADE_GEN",
    "DUNK_ATTEMPT_SHORT_PS": "DUNK_ATTEMPT_GEN",
    "DUNK_ATTEMPT_PS": "DUNK_ATTEMPT_GEN",
    "SHOT_ATTEMPT_2_PT_ONEOFF_PS": "SHOT_ATTEMPT_2_PT_GEN",
    "SHOT_ATTEMPT_2_PT_ONEOFF_HERES_PS": "SHOT_ATTEMPT_2_PT_GEN",
    "SHOT_ATTEMPT_3_PT_ONEOFF_PS": "SHOT_ATTEMPT_3_PT_GEN",
    "SHOT_ATTEMPT_3_PT_ONEOFF_HERES_PS": "SHOT_ATTEMPT_3_PT_GEN",
    "TIP_OFF_SPECIFIC_PLAYER_WINS_PS": "TIP_OFF_GEN",
}

SENTENCE = re.compile(
    r'(<Sentence triggerName="(?P<trigger>[^"]+)" id="(?P<id>\d+)"'
    r'(?P<attrs>[^>]*?)numPhrases="(?P<n>\d+)">)(?P<body>.*?)(</Sentence>)',
    re.S)
PHRASE = re.compile(r"\s*<Phrase\b.*?</Phrase>", re.S)


def phrases(body):
    return PHRASE.findall(body)


def is_silence(phrase):
    return "bSILENCE_" in phrase and "TagBuilderEventRef" not in phrase


def strip(text):
    """The sentences file with every player's name taken out. Returns the new
    text and how many sentences changed."""
    by_trigger = {}
    for m in SENTENCE.finditer(text):
        by_trigger.setdefault(m.group("trigger"), m.group("body"))

    changed = 0

    def rewrite(m):
        nonlocal changed
        body = m.group("body")
        if 'TagName="playerID"' not in body:
            return m.group(0)
        kept = [p for p in phrases(body)
                if 'TagName="playerID"' not in p and 'TagName="numberID"' not in p]
        if not any(not is_silence(p) for p in kept):
            other = by_trigger.get(NAMELESS.get(m.group("trigger"), ""))
            if other is None:
                return m.group(0)   # nothing sensible to say instead: leave it
            kept = phrases(other)
        changed += 1
        tail = body[len(body.rstrip()):]
        head = m.group(1).replace('numPhrases="%s"' % m.group("n"),
                                  'numPhrases="%d"' % len(kept))
        return head + "".join(kept) + tail + "</Sentence>"

    out = SENTENCE.sub(rewrite, text)
    return out, changed


def strip_root(root, say=print):
    """Rewrite one mod root's sentences file. Safe to run again."""
    path = Path(root) / SENTENCES
    if not path.is_file():
        return 0
    raw = path.read_bytes()
    bom = raw.startswith(b"\xef\xbb\xbf")
    text = raw.decode("utf-8-sig")
    if MARK in text:
        return 0
    out, changed = strip(text)
    if not changed:
        return 0
    first_line_end = out.find("\n") + 1
    out = out[:first_line_end] + "  " + MARK + "\n" + out[first_line_end:]
    tmp = path.with_suffix(".xml.tmp")
    tmp.write_bytes((b"\xef\xbb\xbf" if bom else b"") + out.encode("utf-8"))
    os.replace(tmp, path)    # a new file: the base game's copy is untouched
    say("  commentary: %d sentence(s) no longer name a player" % changed)
    return changed


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    strip_root(argv[1])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
