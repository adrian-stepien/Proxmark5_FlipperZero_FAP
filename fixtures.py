"""Reader settings and synthetic fixtures for the bench."""

import re
from dataclasses import dataclass


@dataclass(frozen=True)
class Reader:
    name: str
    position: int
    rescan: str
    pm5_command: str
    placement: str
    usb_fields: dict[str, str]
    timeout: float = 8

    def parse_usb(self, output):
        values = {}
        for key, pattern in self.usb_fields.items():
            matches = {
                "".join(value.split()).upper()
                for value in re.findall(pattern, output, re.MULTILINE | re.IGNORECASE)
            }
            if len(matches) != 1:
                raise ValueError(
                    f"{self.name}: expected one PM5 USB {key}, got {sorted(matches)}"
                )
            values[key] = matches.pop()
        return values


HF = Reader(
    "hf",
    0,
    "RIGHT",
    "hf 14a reader",
    "Place the Chameleon Ultra on the PM5 HF antenna.",
    {
        key.lower(): rf"^.*?\b{key}: *(?:\[\d+\] *)?((?:[\da-f]{{2}} *)+)"
        for key in ("UID", "ATQA", "SAK")
    },
)
READERS = (HF,)


@dataclass(frozen=True)
class Case:
    name: str
    reader: Reader
    emulation: str = ""
    uid: str = ""
    atqa: str = ""
    sak: str = ""

    @property
    def expected(self):
        if not self.emulation:
            return None
        return {
            key: value
            for key in self.reader.usb_fields
            if (value := getattr(self, key))
        }


CASES = (Case("ultralight-7", HF, "MF0ICU1", "041920D37A5E8C", "0044", "00"),)
