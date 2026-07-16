"""
profile_to_flame.py — Parse sandfleaOS profile.log into speedscope JSON.

Profile event format (one per line):
    <timestamp_us>:<type>:<name>\n


    B — Begin (duration start)
    E — End   (duration end)
    I — Instant (single point)

Output: speedscope "evented" profile JSON consumable by any flame graph viewer.
"""

import json
import os
import hashlib


def parse_profile(log_path):
    """Parse profile.log and return {'pairs': [...], 'instants': [...], 'max_us': int}.

    pairs: [{name, start_us, end_us, duration_us, o_seq, c_seq}, ...]
    instants: [{name, at_us, seq}, ...]
    """
    pairs = []
    instants = []
    open_stack = []  # list of (name, start_us, o_seq) — LIFO for nested B/E matching
    seq = 0          # monotonically increasing sequence — preserves log line order

    if not os.path.exists(log_path):
        return {"pairs": pairs, "instants": instants, "max_us": 0}

    max_us = 0

    with open(log_path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue

            # Parse: <timestamp>:<type>:<name>[:<detail>]
            try:
                colon1 = line.index(":")
            except ValueError:
                continue

            try:
                ts = int(line[:colon1])
            except ValueError:
                continue

            rest = line[colon1 + 1:]
            try:
                colon2 = rest.index(":")
            except ValueError:
                continue

            etype = rest[:colon2]
            name = rest[colon2 + 1:]

            if ts > max_us:
                max_us = ts

            if etype == "B":
                open_stack.append((name, ts, seq))
                seq += 1
            elif etype == "E":
                # Pop the most recent unmatched B with matching name.
                # Search from the top of the stack to handle interleaved
                # calls (e.g., B:foo, B:bar, E:bar, E:foo).
                for i in range(len(open_stack) - 1, -1, -1):
                    if open_stack[i][0] == name:
                        _, start, o_seq = open_stack.pop(i)
                        pairs.append({
                            "name": name,
                            "start_us": start,
                            "end_us": ts,
                            "duration_us": ts - start,
                            "o_seq": o_seq,   # sequence of the B event
                            "c_seq": seq,      # sequence of the E event
                        })
                        seq += 1
                        break
            elif etype == "I":
                instants.append({"name": name, "at_us": ts, "seq": seq})
                seq += 1

    # Close any unmatched B events at max_us
    for name, start, o_seq in open_stack:
        pairs.append({
            "name": name,
            "start_us": start,
            "end_us": max_us,
            "duration_us": max_us - start,
            "o_seq": o_seq,
            "c_seq": seq,
        })
        seq += 1

    return {
        "pairs": pairs,
        "instants": instants,
        "max_us": max_us,
    }


def _hash_color(name):
    """Generate a consistent hue from a function name hash."""
    h = int(hashlib.md5(name.encode()).hexdigest()[:8], 16) % 360
    return f"hsl({h}, 65%, 55%)"


def to_speedscope(log_path):
    """Convert profile.log to speedscope evented JSON string.

    B/E pairs become O(pen)/C(lose) events.
    I events become 1µs O/C pairs so they're visible as thin bars.

    Uses sequence numbers (o_seq/c_seq) as tie-breakers when timestamps collide,
    preserving the original log order so nested scopes render correctly.
    """
    data = parse_profile(log_path)
    pairs = data["pairs"]
    instants = data["instants"]
    max_us = max(data["max_us"], 1)

    events = []

    for p in pairs:
        dur = max(p["duration_us"], 1)  # ensure visible (≥1µs)
        events.append({
            "type": "O", "at": p["start_us"], "frame": p["name"],
            "_seq": p["o_seq"],
        })
        events.append({
            "type": "C", "at": p["start_us"] + dur, "frame": p["name"],
            "_seq": p["c_seq"],
        })

    for inst in instants:
        # Instant events → 1µs span so they appear in the flame graph
        at = inst["at_us"]
        label = inst["name"]
        events.append({
            "type": "O", "at": at, "frame": label,
            "_seq": inst["seq"],
        })
        events.append({
            "type": "C", "at": at + 1, "frame": label,
            "_seq": inst["seq"],        # same seq; sort handles type tiebreak below
            "_is_close": True,           # ensures C sorts after O at same (at, seq)
        })

    # Sort by (timestamp, sequence, is_close) — sequence preserves original log
    # order when timestamps collide; is_close ensures C sorts after O at same (at, seq).
    events.sort(key=lambda e: (e["at"], e.get("_seq", 0), e.get("_is_close", False)))

    # Strip internal fields — not part of the speedscope spec
    for e in events:
        e.pop("_seq", None)
        e.pop("_is_close", None)

    # Assign colors to unique frame names
    frames = set()
    for e in events:
        frames.add(e["frame"])

    shared = {
        "frames": [
            {"name": f, "color": _hash_color(f)}
            for f in sorted(frames)
        ]
    }

    profile = {
        "type": "evented",
        "name": "sandfleaOS Profile",
        "unit": "microseconds",
        "startValue": 0,
        "endValue": max_us,
        "events": events,
    }

    return json.dumps({
        "$schema": "https://www.speedscope.app/file-format-schema.json",
        "shared": shared,
        "profiles": [profile],
    })
