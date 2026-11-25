# Proxmark5 for Flipper Zero

## Functions menu

- **HF 14a Reader** and **Hitag2 Reader** show the selected tag's UID.
- **EM4100 Reader** reads the 40-bit EM4100 ID.

To add a simple reader, put its PM5 requests and decoding in a file in
`readers/` and register its `SimpleReader` in `readers/simple_readers.c`.
Return a PM3 status and up to three result lines. The menu uses the registration
table, and the shared screen runs the read and handles errors and rescanning.
