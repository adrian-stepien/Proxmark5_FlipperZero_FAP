"""USB screen capture and button input using Flipper's published RPC schema."""

import re
import struct
import subprocess
import tempfile
import zlib
from collections import deque
from contextlib import ExitStack, contextmanager
from pathlib import Path
from time import monotonic, sleep

from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from serial import Serial

WIDTH, HEIGHT = 128, 64

PROMPT = b">: "


def rpc_message_type(source):
    with tempfile.TemporaryDirectory() as directory:
        descriptors = Path(directory) / "rpc.pb"
        try:
            subprocess.run(
                [
                    "protoc",
                    "-I",
                    str(source.resolve()),
                    "--include_imports",
                    f"--descriptor_set_out={descriptors}",
                    "flipper.proto",
                ],
                check=True,
                timeout=15,
            )
        except subprocess.SubprocessError as exc:
            raise ValueError(
                f"Could not compile Flipper RPC schema from {source}"
            ) from exc
        files = descriptor_pb2.FileDescriptorSet.FromString(descriptors.read_bytes())
    pool = descriptor_pool.DescriptorPool()
    for file in files.file:
        pool.Add(file)
    return message_factory.GetMessageClass(pool.FindMessageTypeByName("PB.Main"))


def varint(value):
    result = bytearray()
    while value >= 0x80:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value)
    return result


def take_message(buffer):
    size = 0
    for index, byte in enumerate(buffer[:3]):
        size |= (byte & 0x7F) << (index * 7)
        if byte < 128:
            start = index + 1
            if size > 65536:
                raise ValueError(f"Flipper RPC message is too large ({size} bytes)")
            if len(buffer) < start + size:
                return None
            packet = bytes(buffer[start : start + size])
            del buffer[: start + size]
            return packet
    if len(buffer) >= 3:
        raise ValueError("Invalid Flipper RPC message length")
    return None


def save_png(path, frame, scale=4):
    def chunk(name, data):
        return (
            struct.pack(">I", len(data))
            + name
            + data
            + struct.pack(">I", zlib.crc32(name + data))
        )

    w, h = WIDTH * scale, HEIGHT * scale
    rows = bytearray()
    for y in range(HEIGHT):
        row = bytearray([0])  # PNG filter type
        for x in range(WIDTH):
            row.extend(
                bytes([255 * (1 - ((frame[(y // 8) * WIDTH + x] >> (y % 8)) & 1))])
                * scale
            )
        rows.extend(row * scale)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


class Flipper:
    def __init__(self, port, app_path, message_type):
        if not re.fullmatch(r"/ext/(?:[\w.-]+/)*[\w.-]+\.fap", app_path, re.ASCII) or (
            {".", ".."} & set(app_path.split("/"))
        ):
            raise ValueError(
                "Use an absolute Flipper .fap path under /ext without spaces"
            )
        self.port = port
        self.app_path = app_path
        self.message_type = message_type

    def _open(self):
        return Serial(self.port, timeout=0.1, write_timeout=3, baudrate=115200)

    @staticmethod
    def _prompt(port, timeout=4):
        message = bytearray()
        deadline = monotonic() + timeout
        while monotonic() < deadline:
            message.extend(port.read_until(PROMPT))
            if message.endswith(PROMPT):
                return message.decode(errors="replace")
        raise TimeoutError("Flipper USB CLI prompt did not arrive")

    @classmethod
    def _cli(cls, port, command):
        port.write(command.encode("ascii") + b"\r")
        deadline = monotonic() + 4
        while (remaining := deadline - monotonic()) > 0:
            output = cls._prompt(port, remaining)
            if output.startswith(command + "\r\n"):
                return output
        raise TimeoutError(f"Flipper CLI did not echo {command!r}")

    @classmethod
    def _sync_cli(cls, port):
        port.reset_input_buffer()
        port.write(b"\x03\r")
        cls._prompt(port)

    def identity(self):
        with self._open() as port:
            self._sync_cli(port)
            info = self._cli(port, "device_info")
            fields = dict(
                re.findall(
                    r"^(firmware_(?:version|api_major|api_minor))\s*:\s*([^\r\n]+)",
                    info,
                    re.MULTILINE,
                )
            )
            output = self._cli(port, f"storage md5 {self.app_path}")
            md5 = re.search(r"\b[\da-f]{32}\b", output, re.IGNORECASE)
            if not md5:
                raise ValueError(f"Could not identify Flipper app at {self.app_path}")
            return fields | {"app_path": self.app_path, "app_md5": md5[0]}

    @contextmanager
    def app(self):
        with self._open() as port, ExitStack() as cleanup:
            self._sync_cli(port)
            if "No application is running" not in self._cli(port, "loader info"):
                raise RuntimeError(
                    "Close the running Flipper app before starting the bench"
                )

            port.write(b"start_rpc_session\r")
            echo = port.read_until(b"\r\n")
            if b"start_rpc_session" not in echo:
                raise RuntimeError(f"Flipper did not enter USB RPC mode: {echo!r}")

            session = FlipperSession(port, self.message_type)
            cleanup.callback(session.request, "stop_session")
            session.request("gui_start_screen_stream_request")
            session.request("app_start_request", name=self.app_path, args="RPC")
            cleanup.callback(session.close_app)
            yield session


class FlipperCleanupError(RuntimeError):
    """The bench could not confirm that the app exited."""


class RpcError(RuntimeError):
    def __init__(self, command, status):
        super().__init__(f"Flipper RPC {command}: status {status}")
        self.status = status


class FlipperSession:
    def __init__(self, port, message_type):
        self.port = port
        self.message_type = message_type
        self.buffer = bytearray()
        self.command_id = 0
        self.latest = None
        self.records = deque()

    def receive(self, deadline):
        while monotonic() < deadline:
            payload = take_message(self.buffer)
            if payload is None:
                self.buffer.extend(self.port.read(self.port.in_waiting or 1))
                continue
            packet = self.message_type.FromString(payload)
            if packet.HasField("gui_screen_frame"):
                frame = packet.gui_screen_frame
                if frame.orientation != 0 or len(frame.data) != WIDTH * HEIGHT // 8:
                    raise ValueError("Expected a 128×64 horizontal Flipper screen")
                self.latest = frame.data
            elif packet.HasField("app_data_exchange_request"):
                self.records.append(packet.app_data_exchange_request.data)
            return packet
        return None

    def request(self, command, **fields):
        self.command_id += 1
        packet = self.message_type(command_id=self.command_id)
        content = getattr(packet, command)
        content.SetInParent()
        for key, value in fields.items():
            setattr(content, key, value)
        data = packet.SerializeToString()
        self.port.write(varint(len(data)) + data)
        deadline = monotonic() + 3
        responses = []
        while (reply := self.receive(deadline)) is not None:
            if reply.command_id != self.command_id:
                continue
            if reply.command_status:
                raise RpcError(command, reply.command_status)
            responses.append(reply)
            if not reply.has_next:
                return responses
        raise TimeoutError(f"Flipper RPC {command} did not reply")

    def close_app(self):
        try:
            try:
                self.request("app_exit_request")
            except RpcError as exc:
                if exc.status != 21:  # ERROR_APP_NOT_RUNNING
                    raise
            deadline = monotonic() + 12
            while monotonic() < deadline:
                (reply,) = self.request("app_lock_status_request")
                if not reply.HasField("app_lock_status_response"):
                    raise ValueError("Missing Flipper app lock status")
                if not reply.app_lock_status_response.locked:
                    return
                sleep(0.05)
            raise TimeoutError("App still running after exit request")
        except (OSError, ValueError, RuntimeError) as exc:
            raise FlipperCleanupError(
                f"Could not close the Flipper app: {exc}"
            ) from exc

    def next_record(self, deadline):
        while not self.records:
            if self.receive(deadline) is None:
                return None
        return self.records.popleft()

    def press(self, key, *, wait_for_screen=False):
        before = self.latest
        for kind in ("PRESS", "SHORT", "RELEASE"):
            self.request("gui_send_input_event_request", key=key, type=kind)
        if wait_for_screen:
            deadline = monotonic() + 4
            while self.latest == before and self.receive(deadline) is not None:
                pass
            if self.latest == before:
                raise TimeoutError(f"Flipper screen did not change after {key}")

    def snapshot(self, path):
        deadline = monotonic() + 0.12
        while self.receive(deadline) is not None:
            pass
        if self.latest is None:
            return None
        save_png(path, self.latest)
        return path.name
