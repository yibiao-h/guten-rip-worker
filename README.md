# guten-rip-worker

`guten-rip-worker` is the print-engine worker used by Yink Studio. It provides
RIP and print-driver operations through the gRPC API in
`proto/rip_worker.proto`.

Gutenprint is kept as a nested submodule at `gutenprint/`. Build and packaging
systems should use this tree for XML driver data and Gutenprint headers.

## License

This project is open source under GPL v2.
