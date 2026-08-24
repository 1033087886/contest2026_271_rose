"""Decide whether a transcript starts with the wake phrase, and strip it.

The wake decision used to be an int8 model on the board. It scored 18175 and
32255 on the same spoken phrase in consecutive runs against a 19660 threshold,
so it fired or missed at random; the cloud transcribed both recordings as
"你好，OpenVela。" Once recognition is a transcript, "was I addressed" is a
string question, which is what this module answers.

The gate requires the device name. "你好" alone is far too common in ordinary
speech: of 30 non-command clips measured on 2026-08-09, one transcribed as
"你好呀。" and would have woken the device if a greeting were enough.

Because a cloud round trip costs 2.5-5.8 s, the intended interaction is one
breath -- "你好openvela，今天天气怎么样" -- rather than wake, wait, then speak.
So the gate also returns the remainder of the transcript, which is the actual
request when the user ran both halves together.
"""

from __future__ import annotations

import re
import unicodedata
from dataclasses import dataclass

# Spellings the ASR actually produces for the English half of the phrase.
# Anchored loosely because the model inserts spaces and mishears the vowels;
# every pattern here was observed in a real transcript or is a near neighbour of
# one. Keep this list evidence-driven rather than speculative.
NAME_PATTERNS: tuple[str, ...] = (
    r"open\s*ve[lr]a",
    r"open\s*vala",
    r"open\s*veila",
    r"open\s*wella",
    r"openvela",
    r"欧盘维拉",
    r"欧朋维拉",
)
GREETING_PATTERNS: tuple[str, ...] = (r"你好", r"您好", r"哈喽", r"hello", r"\bhi\b")

# Punctuation and filler the ASR appends that would otherwise survive into the
# request text after the wake phrase is removed.
_LEADING_JUNK = re.compile(r"^[\s,，。.、!！?？;；:：\-—~～]+")


@dataclass(frozen=True)
class WakeVerdict:
    """Whether the device was addressed, and what was asked."""

    woke: bool
    request: str
    """Transcript remainder after the wake phrase; empty when only the phrase was said."""
    name_matched: bool
    greeting_matched: bool
    matched_text: str
    """The exact substring that matched the device name, for evidence logs."""

    @property
    def has_request(self) -> bool:
        return bool(self.request)


def normalize(transcript: str) -> str:
    """Fold width and case so pattern matching sees one form of each character."""
    return unicodedata.normalize("NFKC", transcript).casefold()


def evaluate(transcript: str) -> WakeVerdict:
    """Applies the wake gate to one final transcript."""
    if not isinstance(transcript, str) or not transcript.strip():
        return WakeVerdict(False, "", False, False, "")

    folded = normalize(transcript)
    name_match = None
    for pattern in NAME_PATTERNS:
        name_match = re.search(pattern, folded)
        if name_match is not None:
            break

    greeting_matched = any(re.search(p, folded) for p in GREETING_PATTERNS)
    if name_match is None:
        return WakeVerdict(False, "", False, greeting_matched, "")

    # The request is whatever follows the device name. Slice the folded string
    # and the original at the same offset: NFKC can change length, so recover
    # the tail from the original only when the lengths still agree.
    tail = folded[name_match.end():]
    if len(folded) == len(transcript):
        tail = transcript[name_match.end():]
    request = _LEADING_JUNK.sub("", tail).strip()

    return WakeVerdict(
        woke=True,
        request=request,
        name_matched=True,
        greeting_matched=greeting_matched,
        matched_text=name_match.group(0),
    )
