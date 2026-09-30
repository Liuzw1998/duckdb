#!/usr/bin/env python3
import argparse
import json
import re
from pathlib import Path


RESULT_FILE = re.compile(r"(?P<label>[BPG])_(?P<query>point|in32|wide|medium_plain|medium_zstd)_t\d+_c\d+\.json$")


def affinity_cpu_count(mask):
    count = 0
    for part in mask.split(","):
        bounds = [int(value) for value in part.split("-")]
        count += bounds[-1] - bounds[0] + 1 if len(bounds) == 2 else 1
    return count


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("results", type=Path)
    parser.add_argument("--affinity", default="48-64")
    args = parser.parse_args()
    allowed_cpus = affinity_cpu_count(args.affinity)
    print("label\tquery\tthreads\tconnections\tqps\tp50_ms\tp95_ms\tcpu_ms_per_query\taffinity")
    result_files = sorted(path for path in args.results.rglob("*.json") if RESULT_FILE.search(path.name))
    if not result_files:
        raise SystemExit(f"no native result JSON files found under {args.results}")

    for path in result_files:
        match = RESULT_FILE.search(path.name)
        data = json.loads(path.read_text())
        if data["affinity"] != args.affinity:
            raise SystemExit(f"{path}: affinity {data['affinity']!r} != {args.affinity!r}")
        cpu_per_second = (data["user_cpu_s"] + data["system_cpu_s"]) / data["elapsed_s"]
        if cpu_per_second > allowed_cpus * 1.05 + 0.05:
            raise SystemExit(f"{path}: CPU seconds/wall seconds {cpu_per_second:.3f} exceeds {allowed_cpus}")
        print(
            "\t".join(
                [
                    match.group("label"),
                    match.group("query"),
                    str(data["threads"]),
                    str(data["connections"]),
                    f"{data['qps']:.3f}",
                    f"{data['p50_ms']:.3f}",
                    f"{data['p95_ms']:.3f}",
                    f"{data['cpu_ms_per_query']:.3f}",
                    data["affinity"],
                ]
            )
        )


if __name__ == "__main__":
    main()
