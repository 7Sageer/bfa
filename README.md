<img src="icon.svg" alt="bfa icon" width="64" height="64">

# bfa

An AI agent running as pure Brainfuck — streaming, history, JSON and MCP
included. The Node host only provides I/O.

## Install

Requires Node.js 20+, `c++` and `stdbuf`. The default runtime is ELVM's
`bfopt` via `stdbuf -o0`:

```sh
curl -fsSL https://raw.githubusercontent.com/shinh/elvm/master/tools/bfopt.cc |
  c++ -std=c++11 -O3 -x c++ - -o bfopt
export PATH="$PWD:$PATH"
```

## Run

```sh
export OPENAI_API_KEY=YOUR_KEY
node bfa.mjs --endpoint https://example.com/v1/chat/completions --model YOUR_MODEL --interactive
```

See `node bfa.mjs --help` for all options.
