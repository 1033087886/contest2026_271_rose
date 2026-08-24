from __future__ import annotations

import argparse
import asyncio
import json
import math
from simulator.device import SimulatedDevice


def nearest_rank(values: list[int], percentile: float) -> int:
    ordered = sorted(values)
    return ordered[max(1, math.ceil(len(ordered) * percentile)) - 1]


async def run_benchmark(uri: str, text: str, turns: int, mode: str = "hot") -> dict:
    if mode not in {"hot", "reconnect"}:
        raise ValueError("mode must be 'hot' or 'reconnect'")
    results = []
    connection_times: list[int] = []
    if mode == "hot":
        async with SimulatedDevice(uri) as device:
            assert device.connect_ready_ms is not None
            connection_times.append(device.connect_ready_ms)
            for _ in range(turns):
                results.append(await device.run_turn(text=text, duration_ms=200))
    else:
        for _ in range(turns):
            async with SimulatedDevice(uri) as device:
                assert device.connect_ready_ms is not None
                connection_times.append(device.connect_ready_ms)
                results.append(await device.run_turn(text=text, duration_ms=200))
    fields = (
        "speech_end_to_asr_final_ms",
        "speech_end_to_first_tts_ms",
        "total_turn_ms",
    )
    metrics = {}
    for field in fields:
        values = [getattr(result, field) for result in results]
        metrics[field] = {
            "p50": nearest_rank(values, 0.50),
            "p95": nearest_rank(values, 0.95),
            "min": min(values),
            "max": max(values),
        }
    metrics["session_connect_ready_ms"] = {
        "samples": len(connection_times),
        "p50": nearest_rank(connection_times, 0.50),
        "p95": nearest_rank(connection_times, 0.95),
        "min": min(connection_times),
        "max": max(connection_times),
    }
    return {
        "mode": mode,
        "turns": turns,
        "metrics_ms": metrics,
        "samples": [item.summary() for item in results],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Benchmark Xiao V WebSocket turns")
    parser.add_argument("--uri", default="ws://127.0.0.1:8765")
    parser.add_argument("--text", default="你好，openvela")
    parser.add_argument("--turns", type=int, default=20)
    parser.add_argument("--mode", choices=("hot", "reconnect"), default="hot")
    args = parser.parse_args()
    if args.turns <= 0:
        parser.error("--turns must be greater than zero")
    report = asyncio.run(run_benchmark(args.uri, args.text, args.turns, args.mode))
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
