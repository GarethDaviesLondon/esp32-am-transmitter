"""Working out what rate a board is talking at, by listening rather than asking.

A board says nothing about its baud rate, so the only method is to open a rate,
listen, and score how much of what came back looks like text. Kept away from
the UI because it is the one piece of this tool worth reusing from a script.
"""

import re
import time

from monitor_config import (
    CONFIDENT_BYTES,
    CONFIDENT_SCORE,
    DEFAULT_LISTEN_SECONDS,
    DEFAULT_PROBE_BAUDS,
)
from monitor_ports import open_port, pulse_reset



def score_sample(data):
    """Score how much a sample looks like readable log output (0.0 to ~1.1).

    At the wrong baud a UART delivers framing noise: unprintable bytes, runs of
    0x00 and 0xFF, and no line structure. At the right one it delivers text.
    """
    total = len(data)
    if total == 0:
        return 0.0, {"printable_ratio": 0.0, "lines": 0, "junk": 0}

    printable = sum(1 for b in data if b in (9, 10, 13) or 32 <= b <= 126)
    ratio = printable / total
    junk = data.count(0) + data.count(0xFF)
    lines = data.count(10)

    score = ratio - 0.5 * (junk / total)
    if lines and ratio > 0.8:
        score += 0.1          # line structure is strong evidence
    if total < 16:
        score *= 0.5          # a handful of bytes proves little

    return max(score, 0.0), {"printable_ratio": ratio, "lines": lines, "junk": junk}


def preview_text(data, limit=160):
    """A one-line, safe-to-display rendering of a sample."""
    text = data.decode("utf-8", errors="replace")
    text = re.sub(r"[\r\n]+", " | ", text)
    text = "".join(ch if (ch.isprintable() or ch == " ") else "." for ch in text)
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) > limit:
        text = text[:limit] + "..."
    return text


def sample_port(device, baud, listen_seconds=DEFAULT_LISTEN_SECONDS, reset=True):
    """Listen to one port at one baud rate and return the raw bytes heard."""
    ser = open_port(device, baud)
    try:
        ser.reset_input_buffer()
        if reset:
            pulse_reset(ser)
        deadline = time.monotonic() + listen_seconds
        chunks = []
        while time.monotonic() < deadline:
            waiting = ser.in_waiting
            data = ser.read(waiting if waiting else 1)
            if data:
                chunks.append(data)
        return b"".join(chunks)
    finally:
        ser.close()


def detect_baud(device, bauds=None, listen_seconds=DEFAULT_LISTEN_SECONDS,
                reset=True, stop_when_confident=True, progress=None, cancel=None):
    """Try each candidate rate in turn and rank them by how readable the output was.

    Returns a list of result dicts, best first. A rate that produced nothing is
    kept in the list with a zero score: silence is a result too, since the
    firmware may simply not be printing anything.
    """
    results = []
    for baud in (bauds or DEFAULT_PROBE_BAUDS):
        if cancel is not None and cancel.is_set():
            break
        if progress:
            progress("Trying {} baud...".format(baud))

        entry = {"baud": baud, "bytes": 0, "score": 0.0, "printable_ratio": 0.0,
                 "sample": "", "error": ""}
        try:
            data = sample_port(device, baud, listen_seconds, reset)
        except Exception as exc:
            entry["error"] = str(exc)
            results.append(entry)
            if progress:
                progress("{} baud: {}".format(baud, exc))
            continue

        score, stats = score_sample(data)
        entry.update({
            "bytes": len(data),
            "score": score,
            "printable_ratio": stats["printable_ratio"],
            "sample": preview_text(data),
        })
        results.append(entry)
        if progress:
            progress("{} baud: {} bytes, score {:.2f}".format(baud, len(data), score))

        if stop_when_confident and score >= CONFIDENT_SCORE and len(data) >= CONFIDENT_BYTES:
            if progress:
                progress("Confident match at {} baud; stopping.".format(baud))
            break

    results.sort(key=lambda r: (-r["score"], -r["bytes"], r["baud"]))
    return results


def summarise_results(results):
    """The one-line verdict shown under the result table, in the GUI and the CLI."""
    if not results:
        return "No result. Detection was stopped before any rate was tried."

    errors = [r for r in results if r["error"]]
    if len(errors) == len(results):
        return ("Could not open the port at any rate: {}. Close whatever is holding it "
                "(an IDE serial monitor, pio device monitor, another tab here) and try again."
                .format(errors[0]["error"]))

    best = results[0]
    if best["score"] >= 0.8 and best["bytes"] >= CONFIDENT_BYTES:
        return "Best match: {} baud.".format(best["baud"])
    if best["bytes"] == 0:
        return ("Every rate was silent. The firmware may not print at boot: reset the board while "
                "probing, or give it something to say.")
    return ("No confident match; judge the samples yourself. Highest score: {} baud."
            .format(best["baud"]))
