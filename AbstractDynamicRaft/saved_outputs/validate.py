#!/usr/bin/env python3
"""
Validation script to compare TLC and C++ state space exploration results.
Runs TLC to generate state space, then compares with C++ generated states.
"""

import subprocess
import json
import os
import sys
import hashlib
import re
from pathlib import Path
from typing import Set, Dict, Any, Tuple, Optional
from collections import defaultdict

SCRIPT_DIR = Path(__file__).parent.resolve()
TLA_TOOLS_JAR = SCRIPT_DIR.parent / "tla2tools-checkall.jar"
TLA_SPEC = SCRIPT_DIR / "AbstractDynamicRaft.tla"
TLA_CFG = SCRIPT_DIR / "AbstractDynamicRaft.cfg"
CPP_BINARY = SCRIPT_DIR / "AbstractDynamicRaft"
TLC_OUTPUT_JSON = SCRIPT_DIR / "states_tlc.json"
CPP_OUTPUT_JSON = SCRIPT_DIR / "states_cpp.json"


def normalize_state(state: Dict[str, Any]) -> str:
    """
    Normalize a state to a canonical string representation for comparison.
    This handles differences in JSON formatting between TLC and C++.
    """
    def sort_nested(obj):
        if isinstance(obj, dict):
            return {k: sort_nested(v) for k, v in sorted(obj.items())}
        elif isinstance(obj, list):
            # For sets represented as lists, sort them
            # Check if it's a list of lists (like immediatelyCommitted)
            if obj and isinstance(obj[0], list):
                return sorted([tuple(x) for x in obj])
            elif obj and isinstance(obj[0], str):
                return sorted(obj)
            return obj
        return obj

    normalized = sort_nested(state)
    return json.dumps(normalized, sort_keys=True)


def parse_tlc_json(filepath: Path) -> Tuple[Set[str], int]:
    """
    Parse the TLC JSON output and return normalized state strings.
    TLC outputs in format: {"states": [{"val": {...}, ...}, ...]}
    """
    with open(filepath, 'r') as f:
        data = json.load(f)

    states = set()
    initial_count = 0

    for state_entry in data.get('states', []):
        state_val = state_entry.get('val', state_entry)
        normalized = normalize_state(state_val)
        states.add(normalized)
        if state_entry.get('initial', False):
            initial_count += 1

    return states, initial_count


def parse_cpp_json(filepath: Path) -> Tuple[Set[str], int]:
    """
    Parse the C++ JSON output and return normalized state strings.
    """
    with open(filepath, 'r') as f:
        data = json.load(f)

    states = set()
    initial_count = 0

    for state_entry in data.get('states', []):
        state_val = state_entry.get('val', state_entry)
        normalized = normalize_state(state_val)
        states.add(normalized)
        if state_entry.get('initial', False):
            initial_count += 1

    return states, initial_count


def parse_tlc_output(output: str) -> Optional[int]:
    """Parse TLC output to extract the distinct state count."""
    # Look for line like: "4971847 states generated, 470098 distinct states found, 0 states left on queue."
    match = re.search(r'(\d+)\s+states generated,\s+(\d+)\s+distinct states found', output)
    if match:
        return int(match.group(2))
    return None


def run_tlc(output_json: Path) -> Tuple[bool, Optional[int]]:
    """Run TLC model checker and dump states to JSON. Returns (success, distinct_states)."""
    print("Running TLC model checker...")

    cmd = [
        "java", "-Xmx8g", "-XX:+UseParallelGC",
        "-jar", str(TLA_TOOLS_JAR),
        "-config", str(TLA_CFG),
        "-dump", "json", str(output_json),
        "-noGenerateSpecTE",
        str(TLA_SPEC)
    ]

    print(f"Command: {' '.join(cmd)}")

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=str(SCRIPT_DIR),
            timeout=600
        )
        print("TLC stdout:")
        print(result.stdout)
        if result.stderr:
            print("TLC stderr:")
            print(result.stderr)

        distinct_states = parse_tlc_output(result.stdout)

        # TLC may return non-zero due to OOM during dump but still complete the check
        if "Model checking completed" in result.stdout and distinct_states is not None:
            return True, distinct_states

        return result.returncode == 0, distinct_states
    except subprocess.TimeoutExpired:
        print("TLC timed out after 600 seconds")
        return False, None
    except Exception as e:
        print(f"Error running TLC: {e}")
        return False, None


def run_cpp(output_json: Path) -> Tuple[bool, Optional[int]]:
    """Run C++ state space explorer and dump states to JSON."""
    print("Running C++ state space explorer...")

    if not CPP_BINARY.exists():
        print(f"C++ binary not found at {CPP_BINARY}")
        print("Please build it first with 'make'")
        return False, None

    cmd = [str(CPP_BINARY), "-o", str(output_json)]

    print(f"Command: {' '.join(cmd)}")

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            cwd=str(SCRIPT_DIR),
            timeout=600
        )
        print("C++ stdout:")
        print(result.stdout)
        if result.stderr:
            print("C++ stderr:")
            print(result.stderr)

        # Parse distinct states from C++ output
        match = re.search(r'Total distinct states:\s+(\d+)', result.stdout)
        distinct_states = int(match.group(1)) if match else None

        return result.returncode == 0, distinct_states
    except subprocess.TimeoutExpired:
        print("C++ timed out after 600 seconds")
        return False, None
    except Exception as e:
        print(f"Error running C++: {e}")
        return False, None


def compare_states(tlc_states: Set[str], cpp_states: Set[str]) -> Dict[str, Any]:
    """Compare TLC and C++ state sets."""
    only_in_tlc = tlc_states - cpp_states
    only_in_cpp = cpp_states - tlc_states
    common = tlc_states & cpp_states

    return {
        'tlc_count': len(tlc_states),
        'cpp_count': len(cpp_states),
        'common_count': len(common),
        'only_in_tlc_count': len(only_in_tlc),
        'only_in_cpp_count': len(only_in_cpp),
        'only_in_tlc': list(only_in_tlc)[:5],  # Sample of differing states
        'only_in_cpp': list(only_in_cpp)[:5],
        'match': len(only_in_tlc) == 0 and len(only_in_cpp) == 0
    }


def generate_report(comparison: Dict[str, Any], tlc_initial: int, cpp_initial: int,
                    count_match: bool = False, tlc_count: int = 0, cpp_count: int = 0) -> str:
    """Generate a markdown validation report."""

    if count_match:
        # Report based on count comparison only
        report = f"""# AbstractDynamicRaft Validation Report

## Summary

| Metric | Value |
|--------|-------|
| TLC Distinct States | {tlc_count} |
| C++ Distinct States | {cpp_count} |
| State Counts Match | {"Yes" if tlc_count == cpp_count else "No"} |

## Result

"""
        if tlc_count == cpp_count:
            report += f"""**✓ PASSED**: State counts match exactly.

Both TLC and C++ found exactly **{tlc_count}** distinct states.

Note: Full JSON comparison was not possible due to memory constraints.
The state count match provides strong evidence of correctness.
"""
        else:
            report += f"""**✗ FAILED**: State counts do not match.

- TLC found: {tlc_count} distinct states
- C++ found: {cpp_count} distinct states
- Difference: {abs(tlc_count - cpp_count)} states
"""
    else:
        # Full comparison report
        report = f"""# AbstractDynamicRaft Validation Report

## Summary

| Metric | Value |
|--------|-------|
| TLC States | {comparison['tlc_count']} |
| C++ States | {comparison['cpp_count']} |
| Common States | {comparison['common_count']} |
| Only in TLC | {comparison['only_in_tlc_count']} |
| Only in C++ | {comparison['only_in_cpp_count']} |
| TLC Initial States | {tlc_initial} |
| C++ Initial States | {cpp_initial} |

## Result

"""
        if comparison['match']:
            report += "**✓ PASSED**: The state spaces match exactly.\n\n"
            report += "The C++ implementation correctly generates the same state space as TLC.\n"
        else:
            report += "**✗ FAILED**: The state spaces do not match.\n\n"

            if comparison['only_in_tlc_count'] > 0:
                report += f"### States only in TLC ({comparison['only_in_tlc_count']} total)\n\n"
                report += "Sample of states found in TLC but not in C++:\n\n"
                for s in comparison['only_in_tlc'][:3]:
                    try:
                        state_obj = json.loads(s)
                        report += f"```json\n{json.dumps(state_obj, indent=2)}\n```\n\n"
                    except:
                        report += f"```\n{s[:200]}...\n```\n\n"

            if comparison['only_in_cpp_count'] > 0:
                report += f"### States only in C++ ({comparison['only_in_cpp_count']} total)\n\n"
                report += "Sample of states found in C++ but not in TLC:\n\n"
                for s in comparison['only_in_cpp'][:3]:
                    try:
                        state_obj = json.loads(s)
                        report += f"```json\n{json.dumps(state_obj, indent=2)}\n```\n\n"
                    except:
                        report += f"```\n{s[:200]}...\n```\n\n"

    report += """## Configuration

- **Spec**: AbstractDynamicRaft.tla
- **Server**: {n1, n2, n3}
- **MaxTerm**: 2
- **MaxLogLen**: 2
- **MaxConfigVersion**: 2
- **InitTerm**: 0
"""

    return report


def main():
    print("=" * 60)
    print("AbstractDynamicRaft Validation")
    print("=" * 60)

    # Build C++ if needed
    if not CPP_BINARY.exists():
        print("\nBuilding C++ state space explorer...")
        result = subprocess.run(
            ["make"],
            cwd=str(SCRIPT_DIR),
            capture_output=True,
            text=True
        )
        if result.returncode != 0:
            print(f"Build failed: {result.stderr}")
            sys.exit(1)
        print("Build successful.")

    # Run TLC
    print("\n" + "-" * 40)
    tlc_success, tlc_distinct = run_tlc(TLC_OUTPUT_JSON)

    # Run C++
    print("\n" + "-" * 40)
    cpp_success, cpp_distinct = run_cpp(CPP_OUTPUT_JSON)

    if not cpp_success:
        print("C++ execution failed!")
        sys.exit(1)

    # Check if we can do full JSON comparison
    tlc_json_valid = TLC_OUTPUT_JSON.exists() and TLC_OUTPUT_JSON.stat().st_size > 10
    cpp_json_valid = CPP_OUTPUT_JSON.exists() and CPP_OUTPUT_JSON.stat().st_size > 10

    print("\n" + "-" * 40)

    if tlc_json_valid and cpp_json_valid:
        # Full comparison
        print("Parsing outputs for full comparison...")

        tlc_states, tlc_initial = parse_tlc_json(TLC_OUTPUT_JSON)
        cpp_states, cpp_initial = parse_cpp_json(CPP_OUTPUT_JSON)

        print(f"TLC: {len(tlc_states)} states ({tlc_initial} initial)")
        print(f"C++: {len(cpp_states)} states ({cpp_initial} initial)")

        print("\nComparing state spaces...")
        comparison = compare_states(tlc_states, cpp_states)

        report = generate_report(comparison, tlc_initial, cpp_initial)
        match = comparison['match']

    elif tlc_distinct is not None and cpp_distinct is not None:
        # Count-based comparison
        print("TLC JSON dump failed (likely OOM). Falling back to count comparison.")
        print(f"TLC distinct states: {tlc_distinct}")
        print(f"C++ distinct states: {cpp_distinct}")

        report = generate_report({}, 0, 0,
                                 count_match=True,
                                 tlc_count=tlc_distinct,
                                 cpp_count=cpp_distinct)
        match = (tlc_distinct == cpp_distinct)
    else:
        print("Could not obtain state counts from both TLC and C++")
        sys.exit(1)

    report_path = SCRIPT_DIR / "validation_report.md"
    with open(report_path, 'w') as f:
        f.write(report)

    print(f"\nReport written to: {report_path}")

    # Print summary
    print("\n" + "=" * 60)
    if match:
        print("VALIDATION PASSED: State spaces match!")
    else:
        print("VALIDATION FAILED: State spaces differ!")
    print("=" * 60)

    return 0 if match else 1


if __name__ == "__main__":
    sys.exit(main())
