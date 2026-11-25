#!/usr/bin/env python3
"""Run the Proxmark5 Flipper reader bench and save its results."""

import argparse
import json
from contextlib import ExitStack
from pathlib import Path
from time import monotonic

from devices import PM5, chameleon_fixture
from fixtures import CASES, READERS
from flipper import Flipper, FlipperCleanupError, rpc_message_type
from reader_log import ReaderLog, read_fields


def capture(stream, log, reader, key, expected, path):
    start = monotonic()
    stream.press(key)
    record = log.next("read", reader.timeout)
    observed = read_fields(reader, record)
    screenshot = stream.snapshot(path)
    return {
        "status": "PASS" if observed == expected else "FAIL",
        "expected": expected,
        "observed": observed,
        "app_status": record["status"],
        "log_sequence": record["seq"],
        "screenshot": screenshot,
        "elapsed_s": round(monotonic() - start, 3),
    }


def read_app(flipper, reader, expected, directory, phases):
    with flipper.app() as stream:
        log = ReaderLog(stream, directory / "flipper.jsonl")
        try:
            log.next("ready", 8)
            stream.press("RIGHT")
            log.next("functions", 4)
            for _ in range(reader.position):
                stream.press("DOWN", wait_for_screen=True)
            for phase, key in (("first", "OK"), ("rescan", reader.rescan)):
                phases[phase] = capture(
                    stream, log, reader, key, expected, directory / f"{phase}.png"
                )
        except (OSError, ValueError):
            stream.snapshot(directory / "last-screen.png")
            raise


def read_reference(pm5, case, directory, result):
    result["usb_status"] = "FAIL"
    try:
        reference = pm5.read(case.reader, directory / "pm5.txt")
    except (ValueError, OSError) as exc:
        result["errors"].append(str(exc))
        return None
    result["usb"] = reference
    if case.expected is not None and reference != case.expected:
        result["errors"].append("PM5 USB output differs from the programmed profile")
    else:
        result["usb_status"] = "PASS"
    return reference


def run_case(case, pm5, flipper, chameleon, directory, result):
    directory.mkdir()
    result.update(status="FAIL", phases={}, errors=[])
    try:
        if case.emulation:
            chameleon.select(case)
        reference = read_reference(pm5, case, directory, result)
        expected = case.expected or reference
        if expected is not None:
            read_app(flipper, case.reader, expected, directory, result["phases"])
    except (OSError, ValueError, RuntimeError) as exc:
        result["errors"].append(f"{type(exc).__name__}: {exc}")
        if isinstance(exc, FlipperCleanupError):
            raise
    finally:
        if case.emulation:
            chameleon.restore()

    if (
        not result["errors"]
        and result["usb_status"] == "PASS"
        and {"first", "rescan"} <= result["phases"].keys()
        and all(phase["status"] == "PASS" for phase in result["phases"].values())
    ):
        result["status"] = "PASS"


def write_report(path, report):
    (path / "results.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )
    lines = [
        "# Proxmark5 reader bench",
        "",
        "| Case | PM5 USB | Flipper first | Flipper rescan | Result |",
        "| --- | --- | --- | --- | --- |",
    ]
    for result in report["cases"]:
        usb = result.get("usb_status", "SKIP")
        if usb != "SKIP":
            usb = f"[{usb}]({result['case']}/pm5.txt)"
        cells = [result["case"], usb]
        for stage in ("first", "rescan"):
            phase = result.get("phases", {}).get(stage)
            if phase and phase["screenshot"]:
                cells.append(
                    f"[{phase['status']}]({result['case']}/{phase['screenshot']})"
                )
            else:
                cells.append(phase["status"] if phase else "—")
        lines.append("| " + " | ".join([*cells, result["status"]]) + " |")
    lines += [
        "",
        "The JSON report contains the USB comparison, app log values and elapsed times.",
    ]
    (path / "README.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def arguments():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--suite",
        choices=("chameleon", *(reader.name for reader in READERS)),
        default="chameleon",
        help="Chameleon mode prompts for placement",
    )
    parser.add_argument(
        "--case",
        action="append",
        choices=[case.name for case in CASES],
        help="Run only the named case. Repeat to select more than one.",
    )
    parser.add_argument("--pm5-client", type=Path, required=True)
    parser.add_argument("--pm5-port", required=True)
    parser.add_argument("--flipper-port", required=True)
    parser.add_argument("--app", default="/ext/.pm5-stream-bench.fap")
    parser.add_argument("--protobuf-source", type=Path, required=True)
    parser.add_argument("--chameleon-source", type=Path)
    parser.add_argument("--chameleon-port")
    parser.add_argument("--slot", type=int, choices=range(1, 9), default=3)
    parser.add_argument(
        "--output", type=Path, required=True, help="New folder for this run"
    )
    args = parser.parse_args()
    cases = [
        case
        for case in CASES
        if (
            (args.suite == "chameleon" and case.emulation)
            or case.reader.name == args.suite
        )
        and (not args.case or case.name in args.case)
    ]
    if not cases:
        parser.error("No cases selected")
    if any(case.emulation for case in cases) and not (
        args.chameleon_source and args.chameleon_port
    ):
        parser.error("Chameleon source and port are required for emulated fixtures")
    return args, cases


def main():
    args, cases = arguments()
    args.output.mkdir(parents=True)
    report = {"cases": []}
    try:
        pm5 = PM5(args.pm5_client, args.pm5_port)
        version = pm5.command("hw version", args.output / "pm5-version.txt")
        if "Communicating with PM3" not in version:
            raise RuntimeError("PM5 USB client did not connect")

        flipper = Flipper(
            args.flipper_port, args.app, rpc_message_type(args.protobuf_source)
        )
        report["flipper"] = flipper.identity()

        with ExitStack() as devices:
            chameleon = None
            if any(case.emulation for case in cases):
                chameleon = devices.enter_context(
                    chameleon_fixture(
                        args.chameleon_source, args.chameleon_port, args.slot
                    )
                )
                report["chameleon"] = {
                    "firmware": chameleon.version,
                    "fixture_slot": args.slot,
                }

            confirmed = {}
            for case in cases:
                group = case.reader.name
                if args.suite == "chameleon" and group not in confirmed:
                    reply = input(
                        f"{case.reader.placement} Enter to start, or s to skip: "
                    )
                    confirmed[group] = reply.strip().lower() != "s"
                result = {"case": case.name, "reader": group, "status": "SKIP"}
                report["cases"].append(result)
                if confirmed.get(group, True):
                    print(f"Running {case.name}", flush=True)
                    run_case(
                        case, pm5, flipper, chameleon, args.output / case.name, result
                    )
                print(f"{result['status']:4} {case.name}", flush=True)
                write_report(args.output, report)
            if all(case["status"] == "SKIP" for case in report["cases"]):
                raise RuntimeError("No cases ran")
    except (
        OSError,
        ValueError,
        RuntimeError,
        ImportError,
        EOFError,
        KeyboardInterrupt,
    ) as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
        print(report["error"], flush=True)
    finally:
        write_report(args.output, report)

    print(args.output / "results.json")
    return int(
        bool(
            report.get("error")
            or any(case["status"] == "FAIL" for case in report["cases"])
        )
    )


if __name__ == "__main__":
    raise SystemExit(main())
