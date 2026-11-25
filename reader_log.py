"""Read sequenced result records from the FAP's opt-in bench log."""

import json
from time import monotonic


class ReaderLog:
    def __init__(self, stream, local_file):
        self.stream = stream
        self.local_file = local_file
        self.sequence = 0

    def next(self, event, timeout):
        raw = self.stream.next_record(monotonic() + timeout)
        if raw is None:
            suffix = ". Use the streaming bench FAP" if event == "ready" else ""
            raise TimeoutError(
                f"No Flipper log event {event!r} at sequence {self.sequence}{suffix}"
            )
        with self.local_file.open("ab") as output:
            output.write(raw)

        record = json.loads(raw)
        expected = {"version": 1, "seq": self.sequence, "event": event}
        if not isinstance(record, dict) or any(
            type(record.get(key)) is not type(value) or record[key] != value
            for key, value in expected.items()
        ):
            raise ValueError(
                f"Unexpected Flipper log record. Expected {expected}, received {record}"
            )
        self.sequence += 1
        return record


def read_fields(reader, record):
    if record.get("reader") != reader.name or type(record.get("status")) is not int:
        raise ValueError(
            f"Unexpected Flipper reader record for {reader.name}: {record}"
        )
    if record["status"] != 0:
        return None
    source = {reader.data_key: record.get("data")} if reader.data_key else record
    values = {key: source.get(key) for key in reader.usb_fields}
    if any(not isinstance(value, str) for value in values.values()):
        raise ValueError(
            f"Missing or invalid Flipper fields for {reader.name}: {record}"
        )
    return reader.result_fields(values)
