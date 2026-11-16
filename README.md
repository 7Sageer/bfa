<img src="icon.svg" alt="bfa icon" width="64" height="64">

# bfa

An AI agent running as pure Brainfuck — streaming, history, JSON and MCP
included. The Node host only provides I/O.

## Run

Requires Node.js 20+, a C compiler and a C++ compiler. The first run builds
the bundled EOF-corrected bfopt interpreter locally; no `stdbuf` is needed.

```sh
export OPENAI_API_KEY=YOUR_KEY
node bfa.mjs --endpoint https://example.com/v1/chat/completions --model YOUR_MODEL --interactive
```

`bfa.bf` is the prebuilt agent. `bfa.eir` is its matching intermediate
representation. See `node bfa.mjs --help` for all options.

## Rebuild

The agent and required ELVM sources are included. Rebuilding writes to the
ignored `build/` directory; copy the outputs to the repository root to publish
them. The native compiler and interpreter are built locally and are not
published as binaries.

```sh
node scripts/build.mjs
cp build/bfa.bf build/bfa.eir .
shasum -a 256 -c SHA256SUMS
```

Source revision: `4d5c8b981d00658373fe9920b223e8d4a49e970d`
(JSON completion checks through parent links). The build uses the `kernels`
frontend, stack-first memory, nibble dispatch and cached EIR forwarding.

## License

The agent and host are MIT-licensed. Bundled ELVM/8cc sources retain their
upstream license notices under `vendor/elvm/`.
