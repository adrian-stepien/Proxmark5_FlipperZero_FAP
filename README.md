# Proxmark5 reader bench

Run the installed Flipper app against a Proxmark5 connected over USB and CEP. The bench sets up each synthetic tag on a Chameleon Ultra, reads it with the PM5 USB client, opens the same reader on the Flipper and rescans. It compares the result records emitted by the FAP when each read finishes. Screenshots show the device view for manual review.

The HF fixtures cover Ultralight, NTAG213, and four- and seven-byte MIFARE Classic UIDs. The ISO 14443-4 cases also check ATS, its CRC and a ten-byte UID. The LF set uses three RF/64 EM4100 IDs.

## Setup

Connect the PM5 and Flipper over the usual CEP cable and connect both to the computer over USB. Connect the Chameleon Ultra over USB and keep it in emulation mode with one spare empty, disabled slot. Use the streaming bench FAP built from the local `dev/fap-reader-log` branch. Close any running Flipper app before the bench starts.

Use Python 3.10 or newer, `protoc`, a Proxmark3 client that matches the PM5 firmware, and checkouts of [Flipper's RPC schema](https://github.com/flipperdevices/flipperzero-protobuf) and the [Chameleon Ultra CLI](https://github.com/RfidResearchGroup/ChameleonUltra/wiki/cli).

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements.txt -r "$CHAMELEON_SOURCE/software/script/requirements.txt"
```

Stage the bench FAP at `/ext/.pm5-stream-bench.fap` with the uFBT storage tool. The example uses shell variables for the three serial ports and paths to the client and the two source checkouts.

```sh
python reader_bench.py \
  --pm5-client "$PM5_CLIENT" --pm5-port "$PM5_PORT" \
  --flipper-port "$FLIPPER_PORT" --protobuf-source "$FLIPPER_PROTOBUF" \
  --chameleon-source "$CHAMELEON_SOURCE" --chameleon-port "$CHAMELEON_PORT" \
  --slot 3 --output "runs/first-run"
```

On the bench Mac, run `.bench-deps/run-local --output runs/first-run`. It uploads the streaming build to a temporary path, verifies the copy and removes it after the run.

The default run asks you to confirm Chameleon placement for each antenna. For an unattended run, place the Chameleon first and use `--suite hf` or `--suite em4100`. Use `--case ultralight-7` to select that fixture and `--app` to select a different copy of the bench build. The tool uses an empty spare slot and restores the previous slot after the run.

## Results

Open the run's `README.md` for separate PM5 USB, Flipper first-read, rescan and ATS results with links to the captures. `results.json` records the firmware and FAP hash, comparison values and elapsed times. Each case keeps its PM5 USB output and original Flipper records in `flipper.jsonl`. The bench launches the app over USB RPC and receives the result records on that connection. The Flipper streams them from memory. The Mac saves the transcripts and screenshots. A failed case or a run where every case was skipped exits with an error.

## Add a case

Add one entry to `CASES` in `fixtures.py`, for example `Case("ultralight-extra", HF, "MF0ICU1", "04237E5A96B108", "0044", "00")`. Run it with `--suite hf --case ultralight-extra`.

For a new reader, its `Reader` entry holds the menu position, rescan button, PM5 command and USB fields. The FAP publishes a matching result record when the read finishes.

```sh
python -m unittest -v
```
