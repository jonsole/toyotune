"""SCPI control of a Rigol MSO5000-series scope (MSO5074) over LAN.

No VISA layer: the scope listens for raw SCPI on TCP port 5555, so this
is plain sockets and the standard library. Runnable on its own --

    python tools/rigol/scope.py 192.168.1.50

prints the *IDN? reply and a summary of the current front-panel setup,
which is the quickest way to prove the scope is reachable before
pointing an MCP client at it.
"""

from __future__ import annotations

import dataclasses
import socket
import sys
import threading

DEFAULT_PORT = 5555

# The scope reports a measurement it cannot make (no signal, or an item
# not applicable to that source) as this magic value rather than as an error.
INVALID_MEASUREMENT = 9.9e37

# :WAV:DATA? will not hand over more than this many points in one go in
# BYTE format; a deep-memory read has to be walked in chunks with
# :WAV:STAR/:WAV:STOP.
MAX_POINTS_PER_READ = 250_000


class ScopeError(Exception):
    """A SCPI command failed, timed out, or came back malformed."""


class Scpi:
    """A line/block oriented SCPI socket, connected lazily."""

    def __init__(self, host: str, port: int = DEFAULT_PORT, timeout: float = 5.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: socket.socket | None = None
        self._buf = b""
        # One connection, possibly several caller threads (the MCP server
        # runs each tool on a worker). Re-entrant so a compound operation
        # can hold the line across many commands while the individual
        # write/query calls inside it take the same lock again.
        self._lock = threading.RLock()

    def transaction(self) -> threading.RLock:
        """Hold the connection for a sequence that must not be interleaved.

        A single query is atomic on its own, but something like a deep
        memory read is a conversation -- :WAV:STAR, :WAV:STOP, :WAV:DATA?
        -- and another caller landing in the middle of it would answer
        the wrong question with the right-looking data.
        """
        return self._lock

    def connect(self) -> None:
        if self._sock is not None:
            return
        try:
            self._sock = socket.create_connection((self.host, self.port), self.timeout)
        except OSError as exc:
            raise ScopeError(f"cannot reach {self.host}:{self.port} -- {exc}") from exc
        self._sock.settimeout(self.timeout)
        self._buf = b""

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None
        self._buf = b""

    def write(self, command: str) -> None:
        with self._lock:
            self.connect()
            assert self._sock is not None
            try:
                self._sock.sendall(command.encode("ascii") + b"\n")
            except OSError as exc:
                self.close()
                raise ScopeError(f"send failed ({command}) -- {exc}") from exc

    def query(self, command: str, timeout: float | None = None) -> str:
        with self._lock:
            self.write(command)
            return self._read_line(timeout).decode("ascii", "replace").strip()

    def query_block(self, command: str, timeout: float | None = None) -> bytes:
        """Query something that answers with an IEEE 488.2 binary block."""
        with self._lock:
            self.write(command)
            return self._read_block(timeout)

    # -- receiving ------------------------------------------------------

    def _fill(self, timeout: float | None) -> None:
        assert self._sock is not None
        self._sock.settimeout(timeout if timeout is not None else self.timeout)
        try:
            chunk = self._sock.recv(65536)
        except socket.timeout as exc:
            raise ScopeError("timed out waiting for a reply") from exc
        except OSError as exc:
            self.close()
            raise ScopeError(f"receive failed -- {exc}") from exc
        if not chunk:
            self.close()
            raise ScopeError("scope closed the connection")
        self._buf += chunk

    def _read_line(self, timeout: float | None = None) -> bytes:
        while b"\n" not in self._buf:
            self._fill(timeout)
        line, _, self._buf = self._buf.partition(b"\n")
        return line

    def _read_exact(self, count: int, timeout: float | None = None) -> bytes:
        while len(self._buf) < count:
            self._fill(timeout)
        data, self._buf = self._buf[:count], self._buf[count:]
        return data

    def _read_block(self, timeout: float | None = None) -> bytes:
        # #<one digit N><N digits of length><that many bytes><newline>
        head = self._read_exact(2, timeout)
        if head[:1] != b"#":
            raise ScopeError(f"expected a binary block, got {head!r}")
        digits = int(head[1:2])
        if digits == 0:
            raise ScopeError("indefinite-length blocks are not supported")
        length = int(self._read_exact(digits, timeout))
        data = self._read_exact(length, timeout)
        if self._buf[:1] == b"\n":
            self._buf = self._buf[1:]
        return data


@dataclasses.dataclass
class Preamble:
    """The ten :WAV:PRE? fields, which scale raw samples into volts/seconds."""

    format: int
    type: int
    points: int
    count: int
    xincrement: float
    xorigin: float
    xreference: float
    yincrement: float
    yorigin: float
    yreference: float


class RigolScope:
    """The MSO5074 itself: setup, measurement, capture and screenshots."""

    def __init__(self, host: str, port: int = DEFAULT_PORT, timeout: float = 5.0):
        self.scpi = Scpi(host, port, timeout)

    @property
    def address(self) -> str:
        return f"{self.scpi.host}:{self.scpi.port}"

    def close(self) -> None:
        self.scpi.close()

    def identify(self) -> dict:
        fields = (self.scpi.query("*IDN?").split(",") + ["", "", "", ""])[:4]
        return {
            "vendor": fields[0],
            "model": fields[1],
            "serial": fields[2],
            "firmware": fields[3],
            "address": self.address,
        }

    def last_error(self) -> str:
        return self.scpi.query(":SYST:ERR?")

    # -- acquisition control --------------------------------------------

    def run(self) -> None:
        self.scpi.write(":RUN")

    def stop(self) -> None:
        self.scpi.write(":STOP")

    def single(self) -> None:
        self.scpi.write(":SING")

    def force_trigger(self) -> None:
        self.scpi.write(":TFOR")

    def autoscale(self) -> None:
        # Autoscale re-hunts the whole front panel and can take seconds.
        self.scpi.write(":AUT")

    def trigger_status(self) -> str:
        return self.scpi.query(":TRIG:STAT?")

    # -- setup ----------------------------------------------------------

    def setup_channel(
        self,
        channel: int,
        enabled: bool | None = None,
        scale: float | None = None,
        offset: float | None = None,
        coupling: str | None = None,
        probe_ratio: float | None = None,
        bandwidth_limit: str | None = None,
    ) -> None:
        with self.scpi.transaction():
            if enabled is not None:
                self.scpi.write(f":CHAN{channel}:DISP {'ON' if enabled else 'OFF'}")
            # Probe ratio first: it rescales what a volts/div value means.
            if probe_ratio is not None:
                self.scpi.write(f":CHAN{channel}:PROB {probe_ratio}")
            if coupling is not None:
                self.scpi.write(f":CHAN{channel}:COUP {coupling.upper()}")
            if bandwidth_limit is not None:
                self.scpi.write(f":CHAN{channel}:BWL {bandwidth_limit.upper()}")
            if scale is not None:
                self.scpi.write(f":CHAN{channel}:SCAL {scale}")
            if offset is not None:
                self.scpi.write(f":CHAN{channel}:OFFS {offset}")

    def setup_timebase(self, scale: float | None = None, offset: float | None = None) -> None:
        with self.scpi.transaction():
            if scale is not None:
                self.scpi.write(f":TIM:MAIN:SCAL {scale}")
            if offset is not None:
                self.scpi.write(f":TIM:MAIN:OFFS {offset}")

    def setup_trigger(
        self,
        source: str | None = None,
        level: float | None = None,
        slope: str | None = None,
        sweep: str | None = None,
    ) -> None:
        with self.scpi.transaction():
            self.scpi.write(":TRIG:MODE EDGE")
            if source is not None:
                self.scpi.write(f":TRIG:EDGE:SOUR {source.upper()}")
            if slope is not None:
                self.scpi.write(f":TRIG:EDGE:SLOP {slope.upper()}")
            if level is not None:
                self.scpi.write(f":TRIG:EDGE:LEV {level}")
            if sweep is not None:
                self.scpi.write(f":TRIG:SWE {sweep.upper()}")

    def setup_logic(
        self,
        channels: list[int] | None = None,
        enabled: bool = True,
        threshold: float | None = None,
    ) -> None:
        """Digital channels D0-D15 -- needs the PLA2216 logic probe attached."""
        with self.scpi.transaction():
            if channels is not None:
                for d in channels:
                    # The channel is an ARGUMENT here, not part of the mnemonic.
                    # The obvious-looking :LA:DIG<n>:DISP is rejected outright
                    # (-104 "Data type err") and changes nothing.
                    self.scpi.write(f":LA:DISP D{d},{'ON' if enabled else 'OFF'}")
            if threshold is not None:
                # Pods are threshold-set as a group: D0-D7, then D8-D15.
                self.scpi.write(f":LA:POD1:THR {threshold}")
                self.scpi.write(f":LA:POD2:THR {threshold}")

    def logic_state(self, channels: list[int] | None = None) -> dict:
        """Read back which digital channels are on, and the pod thresholds."""
        with self.scpi.transaction():
            if channels is None:
                channels = list(range(16))
            shown = {}
            for d in channels:
                shown[f"D{d}"] = self.scpi.query(f":LA:DISP? D{d}") == "1"
            return {
                "displayed": shown,
                "pod1_threshold_v": float(self.scpi.query(":LA:POD1:THR?")),
                "pod2_threshold_v": float(self.scpi.query(":LA:POD2:THR?")),
            }

    def drain_errors(self) -> list[str]:
        """Empty the error queue and return whatever was sitting in it.

        :SYSTem:ERRor? pops one entry off a FIFO, so reading it after a
        command can report some unrelated older failure. Clearing first
        is what makes the next read actually about the next command.
        """
        with self.scpi.transaction():
            drained = []
            for _ in range(32):
                error = self.last_error()
                if error.startswith("0,"):
                    break
                drained.append(error)
            return drained

    def raw(self, command: str, expect_reply: bool) -> dict:
        """Run one arbitrary command with the error queue cleared around it.

        Held as a single transaction: a drain, a command and an error read
        that another caller could interleave with would report somebody
        else's failure as this command's.
        """
        with self.scpi.transaction():
            stale = self.drain_errors()
            reply = None
            if expect_reply:
                reply = self.scpi.query(command)
            else:
                self.scpi.write(command)
            return {
                "command": command,
                "reply": reply,
                "error": self.last_error(),
                "cleared_before_running": stale,
            }

    def state(self) -> dict:
        with self.scpi.transaction():
            channels = []
            for n in (1, 2, 3, 4):
                channels.append(
                    {
                        "channel": n,
                        "enabled": self.scpi.query(f":CHAN{n}:DISP?") in ("1", "ON"),
                        "scale_v_per_div": float(self.scpi.query(f":CHAN{n}:SCAL?")),
                        "offset_v": float(self.scpi.query(f":CHAN{n}:OFFS?")),
                        "coupling": self.scpi.query(f":CHAN{n}:COUP?"),
                        "probe_ratio": float(self.scpi.query(f":CHAN{n}:PROB?")),
                    }
                )
            return {
                "address": self.address,
                "channels": channels,
                "timebase": {
                    "scale_s_per_div": float(self.scpi.query(":TIM:MAIN:SCAL?")),
                    "offset_s": float(self.scpi.query(":TIM:MAIN:OFFS?")),
                },
                "trigger": {
                    "mode": self.scpi.query(":TRIG:MODE?"),
                    "status": self.scpi.query(":TRIG:STAT?"),
                    "sweep": self.scpi.query(":TRIG:SWE?"),
                    "source": self.scpi.query(":TRIG:EDGE:SOUR?"),
                    "level_v": float(self.scpi.query(":TRIG:EDGE:LEV?")),
                    "slope": self.scpi.query(":TRIG:EDGE:SLOP?"),
                },
                "acquire": {
                    "sample_rate_sa_per_s": float(self.scpi.query(":ACQ:SRAT?")),
                    "memory_depth": self.scpi.query(":ACQ:MDEP?"),
                    "type": self.scpi.query(":ACQ:TYPE?"),
                },
            }

    # -- measurement ----------------------------------------------------

    def measure(self, item: str, source: str) -> float | None:
        value = float(self.scpi.query(f":MEAS:ITEM? {item.upper()},{source.upper()}"))
        if value >= INVALID_MEASUREMENT:
            return None
        return value

    # -- waveform capture -----------------------------------------------

    def preamble(self) -> Preamble:
        fields = self.scpi.query(":WAV:PRE?").split(",")
        if len(fields) < 10:
            raise ScopeError(f"short :WAV:PRE? reply: {fields}")
        return Preamble(
            format=int(float(fields[0])),
            type=int(float(fields[1])),
            points=int(float(fields[2])),
            count=int(float(fields[3])),
            xincrement=float(fields[4]),
            xorigin=float(fields[5]),
            xreference=float(fields[6]),
            yincrement=float(fields[7]),
            yorigin=float(fields[8]),
            yreference=float(fields[9]),
        )

    def capture(self, source: str = "CHAN1", mode: str = "RAW", points: int | None = None):
        """Read a waveform back as (times, volts, preamble).

        Mode NORM returns the ~1000 points on screen; RAW returns deep
        memory, which the scope will only hand over while stopped, and
        only MAX_POINTS_PER_READ at a time.
        """
        import numpy as np

        mode = mode.upper()
        with self.scpi.transaction():
            if mode == "RAW":
                self.stop()
            self.scpi.write(f":WAV:SOUR {source.upper()}")
            self.scpi.write(f":WAV:MODE {mode}")
            self.scpi.write(":WAV:FORM BYTE")

            pre = self.preamble()
            total = pre.points
            if points is not None:
                total = min(total, points)
            if total <= 0:
                raise ScopeError("scope reports 0 points -- has it acquired anything yet?")

            raw = bytearray()
            start = 1
            while start <= total:
                stop = min(start + MAX_POINTS_PER_READ - 1, total)
                self.scpi.write(f":WAV:STAR {start}")
                self.scpi.write(f":WAV:STOP {stop}")
                chunk = self.scpi.query_block(":WAV:DATA?", timeout=max(self.scpi.timeout, 20.0))
                if not chunk:
                    raise ScopeError(f"empty waveform chunk at point {start}")
                raw += chunk
                start += len(chunk)

        samples = np.frombuffer(bytes(raw[:total]), dtype=np.uint8).astype(np.float64)
        volts = (samples - pre.yorigin - pre.yreference) * pre.yincrement
        times = (np.arange(volts.size) - pre.xreference) * pre.xincrement + pre.xorigin
        return times, volts, pre

    # -- screen ---------------------------------------------------------

    def screenshot(self) -> bytes:
        """Grab the display as raw image bytes -- a 24-bit BMP.

        MSO5000 firmware (checked on 00.01.03.00.01) accepts every format
        argument the manual suggests -- PNG, JPEG, BMP8, TIFF -- reports
        no error for any of them, and then returns the same 1024x600
        24-bit BMP regardless. So ask plainly and let the caller convert;
        keeping the conversion out of here is what keeps this module to
        the standard library.
        """
        data = self.scpi.query_block(":DISP:DATA?", timeout=max(self.scpi.timeout, 20.0))
        if not data:
            raise ScopeError("scope returned an empty screenshot")
        return data


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(f"usage: {argv[0]} <host>[:port]", file=sys.stderr)
        return 2
    host, _, port = argv[1].partition(":")
    scope = RigolScope(host, int(port) if port else DEFAULT_PORT)
    try:
        for key, value in scope.identify().items():
            print(f"{key:>10}: {value}")
        state = scope.state()
        print(f"\ntimebase: {state['timebase']['scale_s_per_div']} s/div")
        print(
            f" trigger: {state['trigger']['source']} @ {state['trigger']['level_v']} V"
            f" ({state['trigger']['status']})"
        )
        for channel in state["channels"]:
            if channel["enabled"]:
                print(
                    f"    CHAN{channel['channel']}: {channel['scale_v_per_div']} V/div,"
                    f" {channel['coupling']}, x{channel['probe_ratio']}"
                )
    except ScopeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        scope.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
