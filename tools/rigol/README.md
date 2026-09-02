# Rigol MSO5074 MCP server

Drives a Rigol MSO5000-series oscilloscope from an MCP client — set up
channels, arm a trigger, read measurements, pull waveforms into CSV, and
grab the screen as a PNG.

Two files, no VISA layer and no vendor runtime: the scope listens for raw
SCPI on TCP port 5555, so [scope.py](scope.py) is plain sockets and the
standard library, and [mcp_server.py](mcp_server.py) wraps it as tools.

- **[scope.py](scope.py)** — the driver. Importable and runnable on its own.
- **[mcp_server.py](mcp_server.py)** — the MCP server, over stdio.

## Check the scope is reachable

Find its address on the scope itself (**Utility → IO Setting → LAN**), then:

```powershell
.venv-win\Scripts\python.exe tools\rigol\scope.py 192.168.1.50
```

That prints the `*IDN?` reply and the current front-panel setup. If it
prints `cannot reach …`, nothing else here will work — check the address,
and that **Utility → IO Setting → LAN** shows the interface as connected.

## Connect an MCP client

Unlike the emulator's MCP endpoint — which is served by an already-running
`zx_server.exe` ([docs/mcp.md](../../docs/mcp.md)) — this server runs over
stdio, so the client launches it. The repo's `.mcp.json` already registers
it as `rigol`, so opening this workspace in Claude Code picks it up (you'll
be prompted to approve it once). To add it to another client:

```bash
claude mcp add rigol -- .venv-win\Scripts\python.exe tools/rigol/mcp_server.py
```

The scope connection is **lazy**: the server starts fine with the bench
switched off, and only fails when a tool actually reaches for the
instrument. Tell it which scope to talk to in any of three ways, in order
of precedence:

1. `connect("192.168.1.50")` at runtime — also lets you move between scopes,
2. the `RIGOL_ADDR` environment variable (the `env` block in `.mcp.json`),
3. `--address 192.168.1.50` on the command line.

`--capture-dir` (or `RIGOL_CAPTURE_DIR`) sets where `capture()` writes CSVs;
it defaults to a `rigol/` folder in the system temp directory.

**Available tools:**

| Tool | Does |
|---|---|
| `connect(address)` | Point at a scope, `host` or `host:port` (default 5555), and identify it |
| `identify()` | `*IDN?` — vendor, model, serial, firmware |
| `get_state()` | The whole front panel: channels, timebase, trigger + status, sample rate, memory depth |
| `setup_channel(channel, enabled=…, scale=…, offset=…, coupling=…, probe_ratio=…, bandwidth_limit=…)` | One analog channel; only the arguments passed are changed |
| `setup_timebase(scale=…, offset=…)` | Seconds/div and horizontal position |
| `setup_trigger(source=…, level=…, slope=…, sweep=…)` | Edge trigger; `sweep` is AUTO, NORM or SING |
| `setup_logic(channels=…, enabled=…, threshold=…)` | Digital channels D0–D15 (needs the PLA2216 probe) |
| `run()` / `stop()` / `single()` | Acquisition control |
| `force_trigger()` | Trigger once regardless of the condition |
| `autoscale()` | The scope's own autoset — overwrites the whole setup |
| `measure(items, source)` | The scope's measurements: `VPP`, `FREQ`, `PERiod`, `PWIDth`, `RTIMe`, … |
| `capture(source, mode, points, csv_path)` | Waveform → CSV of `time_s,volts`, plus stats and a downsampled preview |
| `screenshot()` | The display as a PNG, exactly as it appears on the front panel |
| `scpi(command, expect_reply=…)` | Raw SCPI escape hatch, with `:SYSTem:ERRor?` reported back |

## Things worth knowing

**`capture()` has two modes.** `NORM` returns the ~1000 points on screen and
is fast. `RAW` returns deep acquisition memory — up to millions of points —
but the scope only hands that over while **stopped**, so `capture()` stops it
for you, and it has to be walked in 250k-point chunks. Samples are written to
CSV rather than returned in full; the tool result carries the path, the sample
interval, min/max/mean/RMS and a 200-point preview.

**`measure()` returning `null`** means the scope could not make that
measurement — usually no signal, or not enough edges in the window — rather
than an error.

**One socket, no lock — issue tool calls sequentially.** Tools run their SCPI
exchange on a worker thread, but every one shares a single connection, so two
concurrent calls can interleave mid-command and read each other's replies.
Nothing serialises them yet.

**Set `probe_ratio` before `scale`.** The probe factor rescales what a
volts/div value means, so a x10 probe declared afterwards moves everything
you just set. `setup_channel()` orders them correctly within one call.

## Protocol decoders — not implemented, but mapped

Deliberately left out until there is a protocol wired up and generating data.
What probing the instrument established, so it need not be repeated:

- The subsystem is **`:BUS<n>`**, not the `:DECoder<n>` the manuals suggest —
  that spelling is rejected outright. Four buses exist, `:BUS1`–`:BUS4`.
- Common controls: `MODE` (RS232, PAR, SPI, IIC …), `DISP`, `FORM`
  (DEC/HEX/BIN/ASC), `LABel`.
- RS232: `TX`, `BAUD`, `DBIT`, `SBIT`, `PAR`, `POL`. Parallel: `SOUR`, `BITX`,
  `WIDT`, `CLK`, `POL`, `NREJ` — set `BITX` to a bit index, then `SOUR` to the
  channel feeding it, once per bit.
- **The decoded results do not appear to be readable over SCPI.**
  `:BUS<n>:EVEN:DATA?`, `:EVEN:TABL?`, `:EVEN<m>?` and `:EVEN:COUN?` are all
  rejected with `-100`; only `:BUS<n>:EVEN?` answers. So the scope can be made
  to *show* a decode, but pulling the packet list into Python looks unavailable
  — leaving a `screenshot()` of the decode row, or `capture()` of the digital
  channels and decoding in Python.

That last point is why a Python-side decoder may be the better investment here:
decoding a captured Z80 bus against this project's own disassembly is something
the scope could not do anyway.

## Tests

[tests/test_rigol_scope.py](../../tests/test_rigol_scope.py) stands up a fake
SCPI instrument on a socket and drives the real driver against it, covering
the parts that are easy to get wrong: IEEE 488.2 block framing, chunked
deep-memory reads, and the preamble arithmetic that turns raw bytes into
volts and seconds.

```powershell
.venv-win\Scripts\python.exe -m pytest tests\test_rigol_scope.py
```

Those tests, and an end-to-end run of the MCP server over stdio, pass against
the fake.

**Verified against a real MSO5074** (`MS5A223003380`, firmware
`00.01.03.00.01`), driven through the MCP protocol: every tool except
`autoscale`, `single` and `force_trigger`. Reads were checked against the front
panel in a screenshot taken at the same moment; `capture()` was checked against
the scope's own measurements of its 1 kHz probe-compensation output (VPP and
VMIN agree exactly, frequency and sample interval agree exactly, and both chunk
joins in a 600k-point deep read show a 0.0000 V step); writes were confirmed by
querying the scope *and* by reading the resulting panel.

### Firmware quirks found that way

Worth knowing if you extend this — all three cost real debugging time:

**The format argument to `:DISP:DATA?` does nothing.** `PNG`, `JPEG`, `BMP8`,
`TIFF`, the three-argument `ON,OFF,PNG` form — all accepted, all reporting
`0,"No error"`, and all returning the same 1,843,254-byte 24-bit BMP of the
1024×600 screen. `screenshot()` asks plainly and `mcp_server.to_png()`
re-encodes (that BMP becomes a ~62 KB PNG).

**A digital channel's number is an argument, not part of the mnemonic.** The
natural-looking `:LA:DIG0:DISP ON` is rejected as `-104,"Data type err"` and
changes nothing; the working form is `:LA:DISP D0,ON`. Thresholds *are*
per-pod mnemonics (`:LA:POD1:THR`), so half of `setup_logic` worked and half
silently did not — which is precisely why it looked correct. `setup_logic()`
now returns `logic_state()`, i.e. what the scope reports, never the request
echoed back.

**`:SYSTem:ERRor?` pops a FIFO.** Read it after a command and you may get an
older, unrelated failure — it will happily blame a working command for a stale
`-100`. `scpi()` drains the queue before running anything, and reports what it
cleared.
