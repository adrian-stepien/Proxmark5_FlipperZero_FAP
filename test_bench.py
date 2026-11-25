import json
import unittest
from contextlib import nullcontext
from pathlib import Path
from tempfile import TemporaryDirectory
from time import monotonic
from types import SimpleNamespace
from unittest.mock import Mock, call, patch

import reader_bench
from devices import PM5, Chameleon
from fixtures import CASES, HF
from flipper import FlipperSession, take_message, varint
from reader_log import ReaderLog


class ReferenceTest(unittest.TestCase):
    def test_usb_failure_keeps_output_without_accepting_a_printed_id(self):
        result = SimpleNamespace(
            returncode=1,
            stdout=b"[+] UID: 02 A9 17 C4\n[+] ATQA: 00 04\n[+] SAK: 08 [2]\n",
        )
        with (
            TemporaryDirectory() as directory,
            patch("devices.subprocess.run", return_value=result),
        ):
            log = Path(directory) / "pm5.txt"
            with self.assertRaisesRegex(ValueError, "exited with status 1"):
                PM5(Path("proxmark3"), "port").read(HF, log)
            self.assertEqual(log.read_bytes(), result.stdout)


class FixtureTest(unittest.TestCase):
    def test_slot_switch_precedes_loading_shared_tag_buffer(self):
        case = CASES[0]
        command = Mock()
        empty = {"hf": 0, "lf": 0}
        command.get_active_slot.side_effect = [0, 2, 0]
        command.get_slot_info.return_value = [empty, empty, empty]
        command.get_enabled_slots.return_value = [empty, empty, empty]
        command.get_device_mode.return_value = False
        data = {
            "uid": bytes.fromhex(case.uid),
            "atqa": bytes.fromhex(case.atqa)[::-1],
            "sak": bytes.fromhex(case.sak),
            "ats": b"",
        }
        command.hf14a_get_anti_coll_data.return_value = data
        enums = SimpleNamespace(
            TagSenseType=SimpleNamespace(HF=2),
            TagSpecificType=SimpleNamespace(MF0ICU1=1100),
        )
        fixture = Chameleon(command, enums, 3, ValueError)
        command.reset_mock()
        fixture.select(case)
        calls = command.mock_calls
        select = call.set_active_slot(3)
        initialize = calls.index(call.set_slot_data_default(3, 1100))
        self.assertLess(calls.index(select), initialize)
        self.assertLess(
            calls.index(select, initialize),
            calls.index(call.hf14a_set_anti_coll_data(**data)),
        )

        command.reset_mock()
        fixture.restore()
        calls = command.mock_calls
        self.assertLess(
            calls.index(call.delete_slot_sense_type(3, 2)),
            calls.index(call.set_active_slot(1)),
        )


class RpcTest(unittest.TestCase):
    def test_fragmented_rpc_buffer_keeps_unread_messages(self):
        payload = bytes(1024)
        buffer = varint(len(payload)) + payload[:400]
        self.assertIsNone(take_message(buffer))
        buffer.extend(payload[400:] + b"\x01x")
        self.assertEqual(take_message(buffer), payload)
        self.assertEqual(take_message(buffer), b"x")
        self.assertFalse(buffer)


class StreamTest(unittest.TestCase):
    def test_keeps_a_record_that_arrives_before_the_command_reply(self):
        record = b'{"version":1,"seq":0,"event":"ready"}\n'
        message = Mock(command_id=0, command_status=0)
        message.HasField.side_effect = lambda field: (
            field == "app_data_exchange_request"
        )
        message.app_data_exchange_request.data = record
        reply = Mock(command_id=1, command_status=0, has_next=False)
        reply.HasField.return_value = False
        message_type = Mock()
        message_type.FromString.side_effect = [message, reply]
        message_type.return_value.SerializeToString.return_value = b"ping"

        session = FlipperSession(Mock(), message_type)
        session.buffer.extend(b"\x01a\x01b")
        self.assertEqual(session.request("system_ping_request"), [reply])
        self.assertEqual(session.next_record(monotonic()), record)


class ReaderLogTest(unittest.TestCase):
    def test_rejects_a_stale_rescan_record(self):
        record = b'{"version":1,"seq":0,"event":"read","reader":"hf","status":-4}\n'
        stream = Mock()
        stream.next_record.return_value = record
        with TemporaryDirectory() as directory:
            path = Path(directory) / "flipper.jsonl"
            log = ReaderLog(stream, path)
            log.next("read", 1)
            with self.assertRaises(ValueError):
                log.next("read", 1)
            self.assertEqual(path.read_bytes(), record * 2)


class RunReportTest(unittest.TestCase):
    def run_mocked(self, directory, reply, fixture, case):
        args = SimpleNamespace(
            output=Path(directory) / "run",
            pm5_client=Path("pm3"),
            pm5_port="pm5",
            flipper_port="flipper",
            app="/ext/app.fap",
            protobuf_source=Path("schema"),
            chameleon_source=Path("source"),
            chameleon_port="chameleon",
            slot=3,
            suite="chameleon",
        )

        def read_app(flipper, reader, expected, folder, phases):
            phases["first"] = {"status": "PASS", "screenshot": None}

        with (
            patch.object(reader_bench, "arguments", return_value=(args, [case])),
            patch.object(reader_bench, "PM5") as pm5,
            patch.object(reader_bench, "Flipper") as flipper,
            patch.object(reader_bench, "rpc_message_type"),
            patch.object(
                reader_bench, "chameleon_fixture", return_value=nullcontext(fixture)
            ),
            patch.object(reader_bench, "read_app", side_effect=read_app),
            patch("builtins.input", return_value=reply),
        ):
            pm5.return_value.command.return_value = "Communicating with PM3"
            pm5.return_value.read.return_value = case.expected
            flipper.return_value.identity.return_value = {}
            status = reader_bench.main()
        return status, json.loads((args.output / "results.json").read_text())

    def test_all_skipped_is_not_a_successful_run(self):
        with TemporaryDirectory() as directory:
            status, report = self.run_mocked(
                directory, "s", Mock(version="test"), CASES[0]
            )
        self.assertEqual(status, 1)
        self.assertIn("No cases ran", report["error"])
        self.assertEqual(report["cases"][0]["status"], "SKIP")

    def test_failed_restore_preserves_completed_read(self):
        fixture = Mock(version="test")
        fixture.restore.side_effect = RuntimeError("slot restore failed")
        with TemporaryDirectory() as directory:
            status, report = self.run_mocked(directory, "", fixture, CASES[0])
        self.assertEqual(status, 1)
        self.assertIn("slot restore failed", report["error"])
        case = report["cases"][0]
        self.assertEqual(case["status"], "FAIL")
        self.assertEqual(case["phases"]["first"]["status"], "PASS")
