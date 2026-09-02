"""An MCP server exposing a Rigol MSO5074 oscilloscope as tools.

Runs over stdio, so an MCP client launches it -- unlike the emulator's
MCP endpoint, which is served by an already-running zx_server.exe. The
scope connection itself is lazy: this process starts fine with the
bench switched off, and only fails when a tool actually reaches for it.

Which scope to talk to comes from, in order: the last connect() call,
the RIGOL_ADDR environment variable, or --address on the command line.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import datetime
import io
import os
import sys
import tempfile
from pathlib import Path

from mcp.server.mcpserver import Image, MCPServer

sys.path.insert(0, str(Path(__file__).resolve().parent))

from scope import DEFAULT_PORT, RigolScope, ScopeError  # noqa: E402

# Enough points to see the shape of a capture in the tool result without
# pushing a megabyte of samples through the model's context.
PREVIEW_POINTS = 200


def to_png(data: bytes) -> bytes:
    """Re-encode a scope screenshot as PNG.

    The scope hands back an uncompressed 24-bit BMP whatever format you
    ask it for -- 1.8 MB, and not something an MCP client will render.
    PNG of the same screen is a couple of percent of that.
    """
    if data.startswith(b"\x89PNG"):
        return data
    from PIL import Image as PILImage

    buffer = io.BytesIO()
    PILImage.open(io.BytesIO(data)).convert("RGB").save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


class Connection:
    """Holds the one scope connection, reconnecting on demand."""

    def __init__(self, address: str | None, timeout: float):
        self.timeout = timeout
        self.scope: RigolScope | None = None
        if address:
            self.set_address(address)

    def set_address(self, address: str) -> RigolScope:
        host, _, port = address.partition(":")
        if self.scope is not None:
            self.scope.close()
        self.scope = RigolScope(host, int(port) if port else DEFAULT_PORT, self.timeout)
        return self.scope

    def get(self) -> RigolScope:
        if self.scope is None:
            raise ScopeError(
                "no scope address configured -- call connect(\"192.168.1.50\") "
                "or set RIGOL_ADDR before starting the server"
            )
        return self.scope


def create_server(connection: Connection, capture_dir: Path) -> MCPServer:
    server = MCPServer(
        "rigol-mso5074",
        instructions=(
            "Tools for a Rigol MSO5000-series oscilloscope (MSO5074: 4 analog "
            "channels, 16 digital with the PLA2216 probe) over raw SCPI on TCP "
            "5555. Someone may be turning knobs on the front panel at the same "
            "time -- read get_state() to see the live setup before changing it. "
            "Captures are written to disk and summarised here rather than "
            "returned in full; scpi() is the escape hatch for anything these "
            "tools do not cover."
        ),
    )

    async def call(func, *args, **kwargs):
        """Run a blocking SCPI exchange off the event loop."""
        return await asyncio.to_thread(func, *args, **kwargs)

    @server.tool()
    async def connect(address: str) -> dict:
        """Point the server at a scope: "host" or "host:port" (default port
        5555). Replaces any previous connection and identifies the instrument
        so a wrong address fails here rather than inside a later tool."""
        scope = connection.set_address(address)
        return await call(scope.identify)

    @server.tool()
    async def identify() -> dict:
        """*IDN? -- vendor, model, serial and firmware of the connected scope."""
        return await call(connection.get().identify)

    @server.tool()
    async def get_state() -> dict:
        """The whole front panel in one read: per-channel enable/scale/offset/
        coupling/probe ratio, timebase, trigger (including whether it is
        currently armed, triggered or stopped), sample rate and memory depth."""
        return await call(connection.get().state)

    @server.tool()
    async def setup_channel(
        channel: int,
        enabled: bool | None = None,
        scale: float | None = None,
        offset: float | None = None,
        coupling: str | None = None,
        probe_ratio: float | None = None,
        bandwidth_limit: str | None = None,
    ) -> dict:
        """Configure one analog channel (1-4). `scale` is volts/div and
        `offset` volts; `coupling` is AC, DC or GND; `probe_ratio` is the
        attenuation factor of the probe (e.g. 10 for a x10 probe -- set it
        first, as it rescales what a volts/div value means); `bandwidth_limit`
        is OFF or e.g. 20M. Only the arguments you pass are changed."""
        scope = connection.get()
        await call(
            scope.setup_channel,
            channel,
            enabled=enabled,
            scale=scale,
            offset=offset,
            coupling=coupling,
            probe_ratio=probe_ratio,
            bandwidth_limit=bandwidth_limit,
        )
        return await call(scope.state)

    @server.tool()
    async def setup_timebase(scale: float | None = None, offset: float | None = None) -> dict:
        """Set the main timebase: `scale` in seconds/div, `offset` in seconds
        (negative shows time before the trigger)."""
        scope = connection.get()
        await call(scope.setup_timebase, scale=scale, offset=offset)
        return await call(scope.state)

    @server.tool()
    async def setup_trigger(
        source: str | None = None,
        level: float | None = None,
        slope: str | None = None,
        sweep: str | None = None,
    ) -> dict:
        """Set up an edge trigger. `source` is CHAN1-CHAN4, D0-D15, AC or EXT;
        `level` is in volts; `slope` is POS, NEG or RFAL; `sweep` is AUTO
        (free-run if nothing triggers), NORM (wait) or SING (one shot)."""
        scope = connection.get()
        await call(scope.setup_trigger, source=source, level=level, slope=slope, sweep=sweep)
        return await call(scope.state)

    @server.tool()
    async def setup_logic(
        channels: list[int] | None = None,
        enabled: bool = True,
        threshold: float | None = None,
    ) -> dict:
        """Turn digital channels D0-D15 on or off and set the logic threshold
        in volts (applied to both 8-channel pods). Needs the PLA2216 logic
        probe -- without it these commands are accepted but show nothing.
        Returns what the scope reports afterwards, not what was asked for."""
        scope = connection.get()
        await call(scope.setup_logic, channels=channels, enabled=enabled, threshold=threshold)
        return await call(scope.logic_state, channels)

    @server.tool()
    async def run() -> dict:
        """Start continuous acquisition (:RUN)."""
        scope = connection.get()
        await call(scope.run)
        return {"trigger_status": await call(scope.trigger_status)}

    @server.tool()
    async def stop() -> dict:
        """Stop acquisition (:STOP). Deep-memory capture() needs this state."""
        scope = connection.get()
        await call(scope.stop)
        return {"trigger_status": await call(scope.trigger_status)}

    @server.tool()
    async def single() -> dict:
        """Arm a single-shot acquisition (:SINGle) -- the scope waits for one
        trigger, captures, and stops. Poll get_state() for trigger status."""
        scope = connection.get()
        await call(scope.single)
        return {"trigger_status": await call(scope.trigger_status)}

    @server.tool()
    async def force_trigger() -> str:
        """Trigger once regardless of the trigger condition (:TFORce)."""
        await call(connection.get().force_trigger)
        return "Trigger forced."

    @server.tool()
    async def autoscale() -> dict:
        """Run the scope's own autoset. Takes a few seconds and overwrites the
        vertical, horizontal and trigger setup on every active channel."""
        scope = connection.get()
        await call(scope.autoscale)
        await asyncio.sleep(3.0)
        return await call(scope.state)

    @server.tool()
    async def measure(items: list[str], source: str = "CHAN1") -> dict:
        """Read the scope's own measurements of a source (CHAN1-CHAN4, D0-D15,
        MATH). Items are names like VPP, VMAX, VMIN, VAVG, VRMS, FREQuency,
        PERiod, PWIDth, NWIDth, PDUTy, RTIMe, FTIMe. A null value means the
        scope could not measure it (usually: no signal, or not enough edges)."""
        scope = connection.get()
        results = {}
        for item in items:
            try:
                results[item.upper()] = await call(scope.measure, item, source)
            except (ScopeError, ValueError) as exc:
                results[item.upper()] = f"error: {exc}"
        return {"source": source.upper(), "measurements": results}

    @server.tool()
    async def capture(
        source: str = "CHAN1",
        mode: str = "NORM",
        points: int | None = None,
        csv_path: str | None = None,
    ) -> dict:
        """Read a waveform back as real time/voltage samples.

        Mode NORM returns the ~1000 points currently on screen; RAW returns
        deep acquisition memory (up to millions of points, and the scope must
        be stopped -- capture() stops it for you). Samples are written to a CSV
        of time_s,volts; this returns the path, the sample rate and interval,
        min/max/mean/RMS, and a downsampled preview so you can see the shape.
        Pass `points` to cap how much of memory is read."""
        import numpy as np

        scope = connection.get()
        times, volts, pre = await call(scope.capture, source, mode, points)

        capture_dir.mkdir(parents=True, exist_ok=True)
        if csv_path is None:
            stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
            path = capture_dir / f"{source.lower()}-{stamp}.csv"
        else:
            path = Path(csv_path)
            path.parent.mkdir(parents=True, exist_ok=True)

        def write_csv() -> None:
            with open(path, "w", newline="") as handle:
                writer = csv.writer(handle)
                writer.writerow(["time_s", "volts"])
                writer.writerows(zip(times.tolist(), volts.tolist()))

        await asyncio.to_thread(write_csv)

        step = max(1, volts.size // PREVIEW_POINTS)
        return {
            "source": source.upper(),
            "mode": mode.upper(),
            "points": int(volts.size),
            "csv_path": str(path),
            "sample_interval_s": pre.xincrement,
            "sample_rate_sa_per_s": 1.0 / pre.xincrement if pre.xincrement else None,
            "duration_s": float(times[-1] - times[0]) if volts.size > 1 else 0.0,
            "start_time_s": float(times[0]),
            "min_v": float(volts.min()),
            "max_v": float(volts.max()),
            "mean_v": float(volts.mean()),
            "rms_v": float(np.sqrt(np.mean(volts**2))),
            "preview_v": [round(v, 4) for v in volts[::step][:PREVIEW_POINTS].tolist()],
        }

    @server.tool()
    async def screenshot() -> Image:
        """Grab the scope's display exactly as it appears on the front panel,
        as a PNG. The fastest way to see what is really going on."""
        data = await call(connection.get().screenshot)
        return Image(data=await asyncio.to_thread(to_png, data), format="png")

    @server.tool()
    async def scpi(command: str, expect_reply: bool | None = None) -> dict:
        """Send a raw SCPI command -- the escape hatch for anything the other
        tools do not cover (:MATH, FFT, decoders, save/recall, ...). By default
        a command ending in '?' is read back and any other is fire-and-forget;
        `expect_reply` overrides that. Also reports :SYSTem:ERRor? afterwards,
        so a rejected command is visible rather than silent."""
        scope = connection.get()
        wants_reply = command.strip().endswith("?") if expect_reply is None else expect_reply
        return await call(scope.raw, command, wants_reply)

    return server


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--address",
        default=os.environ.get("RIGOL_ADDR", ""),
        help="scope as host[:port] (default port 5555); also read from RIGOL_ADDR",
    )
    parser.add_argument("--timeout", type=float, default=5.0, help="SCPI timeout in seconds")
    parser.add_argument(
        "--capture-dir",
        default=os.environ.get("RIGOL_CAPTURE_DIR", str(Path(tempfile.gettempdir()) / "rigol")),
        help="where capture() writes CSVs when no path is given",
    )
    args = parser.parse_args(argv[1:])

    connection = Connection(args.address or None, args.timeout)
    server = create_server(connection, Path(args.capture_dir))
    server.run(transport="stdio")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
