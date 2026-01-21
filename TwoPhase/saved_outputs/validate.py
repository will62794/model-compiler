#!/usr/bin/env python3
"""
Validation script to compare TLC and C++ generated state spaces for TwoPhase spec.
Runs TLC to generate JSON, runs C++ to generate JSON, and compares the state sets.
"""

import json
import subprocess
import os
import sys
import time
import re
from pathlib import Path


SCRIPT_DIR = Path(__file__).parent.resolve()
TLA_FILE = SCRIPT_DIR / "TwoPhase.tla"
CFG_FILE = SCRIPT_DIR / "TwoPhase_4RM.cfg"
TLC_JAR = SCRIPT_DIR.parent / "tla2tools-checkall.jar"
CPP_BINARY = SCRIPT_DIR / "twophase"

TLC_JSON_OUTPUT = SCRIPT_DIR / "tlc_states.json"
CPP_JSON_OUTPUT = SCRIPT_DIR / "cpp_states.json"

# Number of resource managers (must match config and C++ binary)
NUM_RM = 4


def normalize_rm_name(name):
    """Normalize RM names to handle TLC's model value format."""
    if isinstance(name, str):
        # TLC may output "rm1" or "rm1_RM" depending on version
        match = re.match(r"rm(\d+)", name)
        if match:
            return f"rm{match.group(1)}"
    return name


def normalize_state(state_val):
    """
    Normalize a state value for comparison.
    Handles differences in how TLC and C++ represent the same logical state.
    """
    normalized = {}

    # Normalize rmState
    if "rmState" in state_val:
        rm_state = state_val["rmState"]
        normalized["rmState"] = {}
        for rm, st in rm_state.items():
            norm_rm = normalize_rm_name(rm)
            normalized["rmState"][norm_rm] = st

    # Normalize tmState
    if "tmState" in state_val:
        normalized["tmState"] = state_val["tmState"]

    # Normalize tmPrepared (set of RMs)
    if "tmPrepared" in state_val:
        prepared = state_val["tmPrepared"]
        if isinstance(prepared, list):
            normalized["tmPrepared"] = frozenset(normalize_rm_name(rm) for rm in prepared)
        elif isinstance(prepared, dict):
            # TLC might output as dict
            normalized["tmPrepared"] = frozenset(normalize_rm_name(rm) for rm in prepared.keys())
        else:
            normalized["tmPrepared"] = frozenset()

    # Normalize msgs (set of messages)
    if "msgs" in state_val:
        msgs = state_val["msgs"]
        normalized_msgs = set()
        if isinstance(msgs, list):
            for msg in msgs:
                if isinstance(msg, dict):
                    msg_type = msg.get("type")
                    if msg_type == "Prepared":
                        rm = normalize_rm_name(msg.get("rm"))
                        normalized_msgs.add(("Prepared", rm))
                    elif msg_type in ("Commit", "Abort"):
                        normalized_msgs.add((msg_type,))
        normalized["msgs"] = frozenset(normalized_msgs)

    return normalized


def state_to_hashable(normalized_state):
    """Convert normalized state to a hashable tuple for set comparison."""
    rm_state = tuple(sorted(normalized_state.get("rmState", {}).items()))
    tm_state = normalized_state.get("tmState", "")
    tm_prepared = normalized_state.get("tmPrepared", frozenset())
    msgs = normalized_state.get("msgs", frozenset())

    return (rm_state, tm_state, tm_prepared, msgs)


def run_tlc(output_json: Path):
    """Run TLC model checker and dump states to JSON."""
    print("Running TLC model checker...")

    cmd = [
        "java",
        "-XX:+UseParallelGC",
        "-jar", str(TLC_JAR),
        "-config", str(CFG_FILE),
        "-dump", "json", str(output_json),
        "-workers", "1",
        str(TLA_FILE)
    ]

    start = time.time()
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        cwd=SCRIPT_DIR
    )
    duration = time.time() - start

    if result.returncode != 0:
        print("TLC failed!")
        print("STDOUT:", result.stdout)
        print("STDERR:", result.stderr)
        sys.exit(1)

    # Parse TLC output for state count
    state_count = None
    for line in result.stdout.split("\n"):
        if "distinct states found" in line.lower():
            match = re.search(r"(\d[\d,]*)\s+distinct states", line, re.IGNORECASE)
            if match:
                state_count = int(match.group(1).replace(",", ""))
                break

    print(f"TLC completed in {duration:.2f}s")
    if state_count:
        print(f"TLC reported {state_count} distinct states")

    return duration, state_count


def run_cpp(output_json: Path):
    """Run C++ state space generator."""
    print("Running C++ state space generator...")

    if not CPP_BINARY.exists():
        print(f"C++ binary not found at {CPP_BINARY}")
        print("Please run 'make' first.")
        sys.exit(1)

    cmd = [str(CPP_BINARY), "--output", str(output_json)]

    start = time.time()
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        cwd=SCRIPT_DIR
    )
    duration = time.time() - start

    if result.returncode != 0:
        print("C++ generator failed!")
        print("STDOUT:", result.stdout)
        print("STDERR:", result.stderr)
        sys.exit(1)

    # Parse output for state count
    state_count = None
    for line in result.stdout.split("\n"):
        if "States found:" in line:
            match = re.search(r"States found:\s*(\d+)", line)
            if match:
                state_count = int(match.group(1))
                break

    print(f"C++ completed in {duration:.2f}s")
    if state_count:
        print(f"C++ reported {state_count} states")

    return duration, state_count


def load_states(json_path: Path):
    """Load states from JSON file and normalize them."""
    with open(json_path, 'r') as f:
        data = json.load(f)

    states = set()
    initial_states = set()

    for state in data.get("states", []):
        val = state.get("val", {})
        normalized = normalize_state(val)
        hashable = state_to_hashable(normalized)
        states.add(hashable)

        if state.get("initial", False):
            initial_states.add(hashable)

    return states, initial_states


def compare_states(tlc_states, cpp_states, tlc_initial, cpp_initial):
    """Compare two state sets and report differences."""
    only_in_tlc = tlc_states - cpp_states
    only_in_cpp = cpp_states - tlc_states
    common = tlc_states & cpp_states

    print("\n" + "=" * 60)
    print("VALIDATION RESULTS")
    print("=" * 60)
    print(f"TLC states: {len(tlc_states)}")
    print(f"C++ states: {len(cpp_states)}")
    print(f"Common states: {len(common)}")
    print(f"Only in TLC: {len(only_in_tlc)}")
    print(f"Only in C++: {len(only_in_cpp)}")

    # Check initial states
    print(f"\nTLC initial states: {len(tlc_initial)}")
    print(f"C++ initial states: {len(cpp_initial)}")

    success = (only_in_tlc == set() and only_in_cpp == set() and
               tlc_initial == cpp_initial)

    if success:
        print("\n*** VALIDATION PASSED ***")
        print("State spaces are identical!")
    else:
        print("\n*** VALIDATION FAILED ***")
        if only_in_tlc:
            print(f"\nSample states only in TLC (first 5):")
            for s in list(only_in_tlc)[:5]:
                print(f"  {s}")
        if only_in_cpp:
            print(f"\nSample states only in C++ (first 5):")
            for s in list(only_in_cpp)[:5]:
                print(f"  {s}")

    return success


def generate_report(tlc_states, cpp_states, tlc_initial, cpp_initial,
                   tlc_duration, cpp_duration, success):
    """Generate a markdown validation report."""
    report_path = SCRIPT_DIR / "validation_report.md"

    only_in_tlc = tlc_states - cpp_states
    only_in_cpp = cpp_states - tlc_states

    with open(report_path, 'w') as f:
        f.write("# TwoPhase Spec Validation Report\n\n")
        f.write("## Configuration\n")
        f.write("- **Spec**: TwoPhase.tla\n")
        f.write("- **Resource Managers**: 4 (rm1, rm2, rm3, rm4)\n")
        f.write("- **Depth Limit**: Unlimited (full state space)\n\n")

        f.write("## Results Summary\n\n")
        f.write(f"| Metric | TLC | C++ |\n")
        f.write(f"|--------|-----|-----|\n")
        f.write(f"| Total States | {len(tlc_states)} | {len(cpp_states)} |\n")
        f.write(f"| Initial States | {len(tlc_initial)} | {len(cpp_initial)} |\n")
        f.write(f"| Runtime | {tlc_duration:.2f}s | {cpp_duration:.2f}s |\n\n")

        f.write("## State Comparison\n\n")
        f.write(f"- **Common states**: {len(tlc_states & cpp_states)}\n")
        f.write(f"- **Only in TLC**: {len(only_in_tlc)}\n")
        f.write(f"- **Only in C++**: {len(only_in_cpp)}\n\n")

        if success:
            f.write("## Validation Status: PASSED\n\n")
            f.write("The C++ implementation generates the exact same state space as TLC.\n")
        else:
            f.write("## Validation Status: FAILED\n\n")
            f.write("There are differences between TLC and C++ state spaces.\n\n")

            if only_in_tlc:
                f.write("### States only in TLC\n\n")
                f.write("```\n")
                for s in list(only_in_tlc)[:10]:
                    f.write(f"{s}\n")
                f.write("```\n\n")

            if only_in_cpp:
                f.write("### States only in C++\n\n")
                f.write("```\n")
                for s in list(only_in_cpp)[:10]:
                    f.write(f"{s}\n")
                f.write("```\n\n")

    print(f"\nReport written to: {report_path}")
    return report_path


def main():
    print("=" * 60)
    print("TwoPhase Spec Validation: TLC vs C++")
    print("=" * 60)
    print()

    # Run TLC
    tlc_duration, _ = run_tlc(TLC_JSON_OUTPUT)

    print()

    # Run C++
    cpp_duration, _ = run_cpp(CPP_JSON_OUTPUT)

    print()

    # Load and compare states
    print("Loading TLC states...")
    tlc_states, tlc_initial = load_states(TLC_JSON_OUTPUT)

    print("Loading C++ states...")
    cpp_states, cpp_initial = load_states(CPP_JSON_OUTPUT)

    # Compare
    success = compare_states(tlc_states, cpp_states, tlc_initial, cpp_initial)

    # Generate report
    generate_report(tlc_states, cpp_states, tlc_initial, cpp_initial,
                   tlc_duration, cpp_duration, success)

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
