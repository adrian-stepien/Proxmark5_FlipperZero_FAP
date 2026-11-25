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
    data_key: str = ""
    optional_fields: tuple[str, ...] = ()
    timeout: float = 8

    def parse_usb(self, output):
        values = {}
        for key, pattern in self.usb_fields.items():
            matches = {
                "".join(value.split()).upper()
                for value in re.findall(pattern, output, re.MULTILINE | re.IGNORECASE)
            }
            if not matches and key in self.optional_fields:
                continue
            if len(matches) != 1:
                raise ValueError(
                    f"{self.name}: expected one PM5 USB {key}, got {sorted(matches)}"
                )
            values[key] = matches.pop()
        return self.result_fields(values)

    def result_fields(self, values):
        if "config" in values:
            values = values | {"max_block": str((int(values["config"], 16) >> 5) & 7)}
        return values

    def log_expectation(self, reference):
        if "ats" in self.optional_fields:
            ats = reference.get("ats", "")
            return reference | {"ats": ats + crc_a(ats) if ats else ""}
        return reference


HF = Reader(
    "hf",
    0,
    "RIGHT",
    "hf 14a reader",
    "Place the Chameleon Ultra on the PM5 HF antenna.",
    {
        key.lower(): rf"^.*?\b{key}: *(?:\[\d+\] *)?((?:[\da-f]{{2}} *)+)"
        for key in ("UID", "ATQA", "SAK", "ATS")
    },
    optional_fields=("ats",),
)
EM4100 = Reader(
    "em4100",
    2,
    "OK",
    "lf em 410x reader -v",
    "Place the Chameleon Ultra on the PM5 LF coil.",
    {"uid": r"\bEM\s+410x\s+ID\s+([\da-f]{10})\b"},
    data_key="uid",
    timeout=12,
)
HITAG2 = Reader(
    "hitag2",
    1,
    "OK",
    "lf hitag reader",
    "Place only your Hitag2 test tag on the PM5 LF coil.",
    {"uid": r"^.*?\bUID[.:]+ +([\da-f]{8})\b"},
    data_key="uid",
    timeout=12,
)
T5577 = Reader(
    "t5577",
    3,
    "OK",
    "lf t55xx detect",
    "Place only your RF/32 Manchester T5577 test tag on the PM5 LF coil.",
    {"config": r"^.*?\bBlock\s*0[.: ]+([\da-f]{8})\b"},
    data_key="config",
    timeout=12,
)
READERS = (HF, EM4100, HITAG2, T5577)


@dataclass(frozen=True)
class Case:
    name: str
    reader: Reader
    emulation: str = ""
    uid: str = ""
    atqa: str = ""
    sak: str = ""
    ats: str = ""

    @property
    def expected(self):
        if not self.emulation:
            return None
        return {
            key: value
            for key in self.reader.usb_fields
            if (value := getattr(self, key))
        }


CASES = (
    Case("ultralight-7", HF, "MF0ICU1", "041920D37A5E8C", "0044", "00"),
    Case("ultralight-ev1", HF, "MF0UL11", "0431A06C217B08", "0044", "00"),
    Case("ntag210-7", HF, "NTAG_210", "046902B3D1006A", "0044", "00"),
    Case("ntag212-7", HF, "NTAG_212", "042A638000D75E", "0044", "00"),
    Case("ntag213-7", HF, "NTAG_213", "0458A1B9C3D47E", "0044", "00"),
    Case("ntag215-7", HF, "NTAG_215", "04029D735CA100", "0044", "00"),
    Case("ntag216-7", HF, "NTAG_216", "0400629B14C7E0", "0044", "00"),
    Case("classic-4", HF, "MIFARE_1024", "02A917C4", "0004", "08"),
    Case("classic-7", HF, "MIFARE_1024", "02E7A4C18B5309", "0044", "08"),
    Case(
        "iso4-ats",
        HF,
        "HF14A_4",
        "043782B6E9C15A",
        "0044",
        "20",
        "10788070020031C16409976126009000",
    ),
    Case(
        "iso4-uid-10", HF, "HF14A_4", "04E27B58A9C60DF3B475", "0084", "20", "0578807002"
    ),
    Case("em4100-increasing", EM4100, "EM410X", "123456789A"),
    Case("em4100-mixed", EM4100, "EM410X", "02B7D3E405"),
    Case("em4100-high-bit", EM4100, "EM410X", "F10C8A36DE"),
    Case("hitag2-physical", HITAG2),
    Case("t5577-physical", T5577),
)


def crc_a(hex_data):
    value = 0x6363
    for byte in bytes.fromhex(hex_data):
        low = (value ^ byte) & 0xFF
        low = (low ^ (low << 4)) & 0xFF
        value = (value >> 8) ^ (low << 8) ^ (low << 3) ^ (low >> 4)
    return value.to_bytes(2, "little").hex().upper()
