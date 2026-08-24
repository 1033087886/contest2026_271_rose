"""Local wake-word spotting with sherpa-onnx, used as a pre-gate before ASR.

The existing wake decision in [wake_gate.py] is a string test on a cloud
transcript. It is accurate but it charges a full MiMo round trip -- 2.5-5.8 s and
one upload of the user's audio -- for every utterance the microphone picks up.
This module answers the same question from the audio directly, on the host, in
~28 ms per clip.

Measured on the 447-clip ASR-verified corpus in artifacts/kws/mimo-wake-dataset-v2
(2026-08-10, at the 4.0/0.02 operating point): 88.6% recall on Chinese-voiced
wake clips, 96.9% on English-voiced, 1/270 negatives firing. On held-out audio it
fired on 0/15 direct human command recordings and 0/30 post-wake negatives, and
on 3/10 board-microphone captures -- which is every capture in that set that
contains real speech rather than room noise.

**What this does and does not buy.** It is a *pre*-gate, not a replacement: the
transcript gate still runs afterwards and remains the authority on what was said.
And it cannot reject unaddressed *speech*, only silence. Recall is ~93%, and the
misses are acoustically indistinguishable from the hits (see `Verdict.uncertain`),
so a non-detection over speech has to fall through to ASR -- otherwise roughly one
request in twelve would be silently dropped, which is the failure the on-device
int8 model was retired for. Against the verified corpus, where all 270 negatives
are speech, this gate therefore saves zero round trips. Its value is on an
always-on microphone, where most captures are silence and room noise, and as a
fast positive signal (a local hit is known 28 ms in, not 3.5 s in).

If the goal were to reject unaddressed speech locally, this model is not
sufficient and a local ASR would be needed instead.

Two things about this engine are load-bearing and easy to get wrong.

First, the wake word must be spelled phonetically, and the Chinese-only KWS
model cannot spell it at all. `text2token` in `ppinyin` mode silently returns an
empty list for "你好openvela" (pypinyin renders `openvela` as `openüela`, which
is not in the token table), so a keyword file built that way looks fine and
contains nothing. Even with Chinese homophones substituted ("你好欧盆维拉"), the
wenetspeech model tops out at 23% recall no matter how the threshold is tuned --
it has no way to represent an English name. The bilingual
`kws-zipformer-zh-en-3M-2025-12-20` model does, via CMU phonemes from its
`en.phone` dictionary, which is why this module requires that model and refuses
to start on the Chinese-only one. `OPENVELA` is not in `en.phone` upstream, so
`_PRONUNCIATIONS` below appends it.

Second, sherpa-onnx has no NuttX build path, so none of this can move to the
board; see docs/kws-engine-survey.md. This runs host-side only.
"""

from __future__ import annotations

import logging
import shutil
import tempfile
from dataclasses import dataclass
from pathlib import Path

LOGGER = logging.getLogger(__name__)

# Directory name of the bilingual KWS model inside the models/ tree. The
# Chinese-only wenetspeech model is deliberately not accepted: it cannot encode
# an English device name (23% recall ceiling, measured 2026-08-10).
MODEL_DIR_NAME = "sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20"
_EPOCH_TAG = "epoch-13-avg-2-chunk-16-left-64"

# CMU-style pronunciations appended to the model's en.phone. "OPENVELA" is not a
# dictionary word; these spellings cover how the name is actually voiced, taking
# OPEN (OW1 P AH0 N) from the shipped dictionary and varying the stressed vowel
# of the second half. Each one that fires in evaluation earns its place here --
# OPENVEILA is the variant that matches real board-microphone recordings.
_PRONUNCIATIONS: tuple[tuple[str, str], ...] = (
    ("OPENVELA", "OW1 P AH0 N V EH1 L AH0"),
    ("OPENVEILA", "OW1 P AH0 N V EY1 L AH0"),
    ("OPENVALA", "OW1 P AH0 N V AA1 L AH0"),
    ("OPENVERA", "OW1 P AH0 N V EH1 R AH0"),
    ("OPENWELLA", "OW1 P AH0 N W EH1 L AH0"),
)

# Phrases to spot. Both halves ("你好 OPENVELA") and the bare name, because users
# run the greeting together with the request or skip it entirely.
_PHRASES: tuple[str, ...] = (
    "你好 OPENVELA",
    "你好 OPENVEILA",
    "你好 OPENVALA",
    "你好 OPENVERA",
    "你好 OPENWELLA",
    "OPENVELA",
    "OPENVEILA",
    "HELLO OPENVELA",
)

# Boost and per-keyword threshold, tuned for a *pre-gate*, where the two errors
# cost wildly different amounts: a false accept costs one cloud round trip that
# would have happened anyway, while a miss can drop a real request. So this sits
# at the recall peak rather than at zero false accepts. Measured on the verified
# corpus (2026-08-10): 4.0/0.02 gives 88.6% zh / 96.9% en recall with 1/270
# negatives firing. Pushing boost higher trades English recall away without
# buying Chinese recall -- the ceiling is ~93% overall.
_KEYWORDS_SCORE = 4.0
_KEYWORDS_THRESHOLD = 0.02

SAMPLE_RATE_HZ = 16000


class LocalKwsUnavailable(RuntimeError):
    """Raised when the engine or model is missing, so the caller can fail open."""


@dataclass(frozen=True)
class Verdict:
    """Outcome of running the local spotter over one turn's audio."""

    fired: bool
    """True when a wake phrase was spotted in the audio."""
    matched: str
    """The phrase tag that fired, for evidence logs; empty when nothing fired."""
    uncertain: bool
    """
    True when a non-detection is not trustworthy enough to end the turn on.

    Recall peaks at ~93%, so roughly one addressed utterance in twelve does not
    fire, and the misses cannot be told apart from the hits by loudness: over the
    177 verified positives the 12 misses had median peak 0.740 and median RMS
    0.118 against 0.745 and 0.137 for the hits, and no peak threshold separates
    them at all (measured 2026-08-10). There is therefore no audio heuristic that
    can label a miss as safe.

    So this is set for anything that carries speech, and only genuine silence is
    reported as a confident negative. That makes the pre-gate a saving on silence
    and room noise rather than on unaddressed speech -- see the module note on
    what this does and does not buy.
    """


def _model_paths(models_root: Path) -> tuple[Path, Path, Path, Path, Path]:
    base = models_root / MODEL_DIR_NAME
    return (
        base / "tokens.txt",
        base / f"encoder-{_EPOCH_TAG}.onnx",
        base / f"decoder-{_EPOCH_TAG}.onnx",
        base / f"joiner-{_EPOCH_TAG}.onnx",
        base / "en.phone",
    )


def model_available(models_root: Path) -> bool:
    """Whether every file the spotter needs is present."""
    return all(p.exists() for p in _model_paths(models_root))


def _augmented_lexicon(en_phone: Path, workdir: Path) -> Path:
    """Copy the model's en.phone and append the device-name pronunciations."""
    target = workdir / "en_augmented.phone"
    shutil.copyfile(en_phone, target)
    with target.open("a", encoding="utf-8") as handle:
        for word, phones in _PRONUNCIATIONS:
            handle.write(f"{word} {phones}\n")
    return target


def _keywords_file(tokens: Path, lexicon: Path, workdir: Path) -> Path:
    """Encode the wake phrases into a sherpa-onnx keywords file.

    Asserts that every phrase produced tokens. `text2token` reports an
    unencodable phrase by dropping it and printing a warning to stdout, so
    without this check a keyword file can come out short -- or empty -- while
    looking correctly formed.
    """
    from sherpa_onnx import text2token

    lines: list[str] = []
    for phrase in _PHRASES:
        encoded = text2token(
            [phrase],
            tokens=str(tokens),
            tokens_type="phone+ppinyin",
            lexicon=str(lexicon),
        )
        if not encoded or not encoded[0]:
            raise LocalKwsUnavailable(
                f"wake phrase {phrase!r} encoded to nothing; the lexicon or "
                "token table does not cover it"
            )
        tag = phrase.replace(" ", "_")
        lines.append(
            f"{' '.join(encoded[0])} :{_KEYWORDS_SCORE} #{_KEYWORDS_THRESHOLD} @{tag}"
        )

    if len(lines) != len(_PHRASES):
        raise LocalKwsUnavailable("keyword encoding dropped phrases")

    target = workdir / "keywords.txt"
    target.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return target


# Below this peak amplitude a clip is treated as containing no speech at all, so
# a miss is a real negative rather than an uncertain one. Every verified positive
# peaked at 0.435 or above, so this leaves a wide margin: the point is to catch
# dead air, not to judge quiet speech. Expressed on the float scale the engine
# consumes.
_SILENCE_PEAK = 0.02
# A 20 ms frame counts as active when it rises above this fraction of the clip
# peak. Relative rather than absolute because board gain varies by ~13 dB
# between captures.
_ACTIVE_FRAME_RATIO = 0.08
_MIN_ACTIVE_FRAMES = 8


class LocalWakeSpotter:
    """Streaming wake-word spotter over 16 kHz mono PCM.

    Construct once and reuse; loading the three ONNX graphs is the expensive
    part. `evaluate` is independent per call -- it builds a fresh stream, so
    turns cannot leak decoder state into each other.
    """

    def __init__(self, models_root: Path, num_threads: int = 2) -> None:
        tokens, encoder, decoder, joiner, en_phone = _model_paths(models_root)
        missing = [str(p) for p in (tokens, encoder, decoder, joiner, en_phone) if not p.exists()]
        if missing:
            raise LocalKwsUnavailable(
                "bilingual KWS model is not installed; missing " + ", ".join(missing)
            )
        try:
            import sherpa_onnx
        except ImportError as exc:  # pragma: no cover - depends on environment
            raise LocalKwsUnavailable(f"sherpa-onnx is not installed: {exc}") from exc

        # Keep the generated lexicon and keyword file for the process lifetime:
        # sherpa-onnx reads them at construction, but holding the directory also
        # makes the encoding reproducible if the spotter is rebuilt.
        self._workdir = Path(tempfile.mkdtemp(prefix="xiaov-kws-"))
        lexicon = _augmented_lexicon(en_phone, self._workdir)
        keywords = _keywords_file(tokens, lexicon, self._workdir)

        self._spotter = sherpa_onnx.KeywordSpotter(
            tokens=str(tokens),
            encoder=str(encoder),
            decoder=str(decoder),
            joiner=str(joiner),
            keywords_file=str(keywords),
            num_threads=num_threads,
            provider="cpu",
            keywords_score=_KEYWORDS_SCORE,
            keywords_threshold=_KEYWORDS_THRESHOLD,
        )
        LOGGER.info("local wake spotter ready (%d phrases)", len(_PHRASES))

    def evaluate_pcm(self, pcm: bytes) -> Verdict:
        """Run the spotter over one turn of 16-bit little-endian mono PCM."""
        import numpy as np

        if len(pcm) < 2:
            return Verdict(False, "", uncertain=False)
        samples = np.frombuffer(pcm[: len(pcm) // 2 * 2], dtype="<i2")
        return self.evaluate_samples(samples.astype(np.float32) / 32768.0)

    def evaluate_samples(self, samples) -> Verdict:
        """Run the spotter over float32 samples in [-1, 1] at 16 kHz."""
        import numpy as np

        if samples.size == 0:
            return Verdict(False, "", uncertain=False)

        stream = self._spotter.create_stream()
        stream.accept_waveform(SAMPLE_RATE_HZ, samples)
        # The zipformer needs padding past the end of speech to emit a decision
        # for a phrase that runs to the last sample. Without this tail, clips
        # where the wake word is the final word never fire.
        stream.accept_waveform(
            SAMPLE_RATE_HZ, np.zeros(int(SAMPLE_RATE_HZ * 0.6), dtype=np.float32)
        )
        stream.input_finished()

        matched = ""
        while self._spotter.is_ready(stream):
            self._spotter.decode_stream(stream)
            result = self._spotter.get_result(stream)
            if result:
                matched = result
                self._spotter.reset_stream(stream)
                break

        if matched:
            return Verdict(True, matched, uncertain=False)
        return Verdict(False, "", uncertain=_has_speech(samples))

    def close(self) -> None:
        shutil.rmtree(self._workdir, ignore_errors=True)


def _has_speech(samples) -> bool:
    """Whether a clip holds enough energy that a non-detection is unreliable."""
    import numpy as np

    peak = float(np.abs(samples).max())
    if peak < _SILENCE_PEAK:
        return False
    frame = int(SAMPLE_RATE_HZ * 0.02)
    usable = samples[: samples.size // frame * frame]
    if usable.size == 0:
        return False
    frames = usable.reshape(-1, frame)
    energies = np.sqrt((frames.astype(np.float64) ** 2).mean(axis=1))
    active = int((energies > peak * _ACTIVE_FRAME_RATIO).sum())
    return active >= _MIN_ACTIVE_FRAMES
