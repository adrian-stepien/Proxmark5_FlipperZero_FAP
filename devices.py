"""Chameleon fixture control and the PM5 USB reference reader."""

import re
import subprocess
import sys
from contextlib import contextmanager
from importlib import import_module

from fixtures import HF


@contextmanager
def chameleon_fixture(source, port, slot):
    scripts = str(source.resolve() / "software" / "script")
    sys.path.insert(0, scripts)
    try:
        command = import_module("chameleon_cmd").ChameleonCMD
        transport = import_module("chameleon_com").ChameleonCom
        enums = import_module("chameleon_enum")
        errors = import_module("chameleon_utils")
        device = transport().open(port)
        try:
            try:
                yield Chameleon(
                    command(device), enums, slot, errors.UnexpectedResponseError
                )
            except errors.UnexpectedResponseError as exc:
                raise RuntimeError(f"Chameleon: {exc}") from exc
        finally:
            device.close()
    finally:
        sys.path.remove(scripts)


class Chameleon:
    def __init__(self, command, enums, slot, response_error):
        self.command = command
        self.enums = enums
        self.slot = slot
        self.active = command.get_active_slot() + 1
        self.types = command.get_slot_info()
        self.enabled = command.get_enabled_slots()
        self.sense = None
        self.response_error = response_error
        if command.get_device_mode():
            raise ValueError("Put the Chameleon in emulation mode first")
        if (
            self.active == slot
            or any(self.types[slot - 1].values())
            or any(self.enabled[slot - 1].values())
        ):
            raise ValueError(
                f"Chameleon slot {slot} must be empty, disabled and inactive"
            )
        self.version = command.get_git_version()

    def select(self, case):
        try:
            self._select(case)
        except self.response_error as exc:
            raise ValueError(f"Chameleon rejected {case.name}: {exc}") from exc

    def _select(self, case):
        if case.reader is not HF:
            raise ValueError(f"No Chameleon fixture for {case.reader.name}")
        cmd = self.command
        band = self.enums.TagSenseType.HF
        kind = getattr(self.enums.TagSpecificType, case.emulation)
        uid = bytes.fromhex(case.uid)
        self.sense = band
        # Changing a slot's type loads it into the device's shared tag buffer.
        # Switch away from the user's profile before loading the fixture.
        cmd.set_active_slot(self.slot)
        if cmd.get_active_slot() + 1 != self.slot:
            raise ValueError("Chameleon did not select the fixture slot")
        try:
            cmd.set_slot_data_default(self.slot, kind)
        except self.response_error as exc:
            raise ValueError(
                f"Chameleon could not initialize slot {self.slot} as {case.emulation}: {exc}"
            ) from exc
        cmd.set_slot_tag_type(self.slot, kind)
        cmd.set_slot_enable(self.slot, band, True)

        # Selecting a slot saves its current data. Arm it before setting the test ID.
        cmd.set_active_slot(self.slot)
        data = {
            "uid": uid,
            "atqa": bytes.fromhex(case.atqa)[::-1],
            "sak": bytes.fromhex(case.sak),
            "ats": b"",
        }
        cmd.hf14a_set_anti_coll_data(**data)

        if cmd.hf14a_get_anti_coll_data() != data:
            raise ValueError("Chameleon fixture readback differs from programmed data")

    def restore(self):
        if self.sense is None:
            return
        cmd = self.command
        try:
            try:
                cmd.delete_slot_sense_type(self.slot, self.sense)
            finally:
                cmd.set_active_slot(self.active)
            if self.active != cmd.get_active_slot() + 1 or (
                self.types,
                self.enabled,
            ) != (cmd.get_slot_info(), cmd.get_enabled_slots()):
                raise RuntimeError("slot inventory differs from the starting state")
        except (self.response_error, OSError, RuntimeError) as exc:
            raise RuntimeError(
                f"Could not restore Chameleon slot {self.active}: {exc}"
            ) from exc
        self.sense = None


ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")


class PM5:
    def __init__(self, client, port):
        self.client = str(client.resolve())
        self.port = port

    def command(self, command, logfile):
        try:
            result = subprocess.run(
                [self.client, self.port, "-c", command],
                check=False,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=20,
            )
        except subprocess.TimeoutExpired as exc:
            logfile.write_bytes(exc.stdout or b"")
            raise TimeoutError(f"PM5 USB {command!r} timed out") from exc
        output = ANSI.sub("", result.stdout.decode(errors="replace"))
        if command == "hw version":
            output = re.sub(r"(?m)^.*Processor Unique ID.*$", "", output)
        logfile.write_text(output, encoding="utf-8")
        if result.returncode:
            raise ValueError(
                f"PM5 USB {command!r} exited with status {result.returncode}"
            )
        return output

    def read(self, reader, logfile):
        return reader.parse_usb(self.command(reader.pm5_command, logfile))
