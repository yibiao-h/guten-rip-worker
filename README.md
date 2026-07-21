# guten-rip-worker

`guten-rip-worker` is the print-engine worker used by Yink Studio. It provides
RIP and print-driver operations through the gRPC API in
`proto/rip_worker.proto`.

The worker is intentionally a narrow bridge: the client supplies resolved,
typed print-engine settings and streaming RAW rows, and the worker returns the
print engine's PRN bytes unchanged. Product policy, channel geometry, white-ink
logic, file I/O, and PRN post-processing belong to the client process.

Gutenprint is kept as a nested submodule at `gutenprint/`. Build and packaging
systems should use this tree for XML driver data and Gutenprint headers.

## License

This project is open source under GPL v2.
