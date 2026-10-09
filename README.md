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

The worker implementation in `src/worker/` and its build integration in
`CMakeLists.txt` and `cmake/` are licensed under the GNU General Public License,
version 3 or (at your option) any later version (`GPL-3.0-or-later`). The RPC
schema in `proto/rip_worker.proto` and the shared RPC interfaces generated from
it are separately available under the MIT License.

The complete license texts are [GPLv3](LICENSES/GPL-3.0-or-later.txt) and
[MIT](LICENSES/MIT.txt). Third-party components, including `gutenprint/` and the
linked dependency libraries, retain their original copyright and license
notices. These grants do not withdraw rights granted for earlier versions.
