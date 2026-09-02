#!/usr/bin/env python3
"""tools/fcade-replays/auto-convert/proxy_ops.py

Mac-side client for fcade-proxy's Mac-worker ops: `worklease`, `workdone`,
`workstats` (docs/plan-preconvert-fleet.md §2 "Mac worker protocol", Q2;
Stage S5). These are the ONLY ops this client speaks -- it never touches
`search`/`status`/`get3sr`/`convert`/`convertstatus`/`watchpoll` (those are
the device's ops, unchanged and untouched by this tool).

Wire protocol (tools/fcade-proxy/README.md "Wire protocol"; fcade-proxy.js
`encodeFrame`/socket `data` handler): one connection, u32be length-prefix +
UTF-8 JSON per frame, request then response, lock-step. `encode_frame` /
`try_decode_frame` below are a straight, from-scratch port of that framing
(the same shape as `fcade_replay_tool.py`'s `_u32be`/`recv_frame`, adapted
from the ggpo-stream's binary records to this proxy's plain JSON frames) --
pure functions, no I/O, exercised offline by `--selftest` (no network, no
subprocess).

Transport: the proxy binds 127.0.0.1:3479 on the VPS only (never the public
interface, even though the host's firewall happens to be open -- every
mutating op stays off the public port on principle, docs/plan-preconvert-
fleet.md §1.4 + Q2). We reach it the same way the existing catalog rail
proves connectivity ('speak the wire protocol over SSH': refresh-catalog.sh's
`verify.js` step, push-3sr.sh's ssh precedent) but via OpenSSH's own stdio-
forwarding primitive instead of a remote helper script: `ssh -W
127.0.0.1:<port> <host>` pipes this process's stdin/stdout directly to the
TCP connection sshd makes, on the VPS, to its own loopback port -- no `nc`,
no remote script, nothing new to provision on the VPS.

Usage:
  proxy_ops.py --selftest
      Pure offline self-test of the framing code. No args, no network,
      no subprocess. Exits 0 on pass.

  proxy_ops.py worklease  --worker NAME [--count N] [--print-only] [transport opts]
  proxy_ops.py workdone   --worker NAME --quarkid ID --ok true|false [--reason R] [--print-only] [transport opts]
  proxy_ops.py workstats  [--print-only] [transport opts]

  transport opts: [--host HOST] [--port PORT] [--ssh-timeout SECS]
                  [--response-timeout SECS] [--token TOK | --token-file PATH]

Token: read (highest priority first) from --token, else --token-file, else
the FCADE_WORK_TOKEN env var. Never hardcoded, never logged/printed in full
(printed requests mask it) -- see auto-convert/README.md "Token
configuration" for where the value itself lives on disk.

On success, prints the response JSON (one line) to stdout and exits 0 iff
the response's "ok" field is true; on any transport/protocol error, prints a
one-line diagnostic to stderr and exits 1. `--print-only` builds the request
and prints it (token masked) WITHOUT creating any subprocess or touching the
network at all -- this is what preconvert-worker.sh's `--dry-run` mode uses
to prove exact command construction with zero side effects.
"""

from __future__ import annotations

import argparse
import json
import os
import select
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional

DEFAULT_HOST = "hetzner-3s-arm"
DEFAULT_PORT = 3479
DEFAULT_SSH_CONNECT_TIMEOUT_S = 10
DEFAULT_RESPONSE_TIMEOUT_S = 20.0
# Mirrors fcade-proxy.js's MAX_FRAME_BYTES (fcade-proxy.js:64) so an
# oversized request is rejected locally instead of wasting an SSH round trip
# on a guaranteed `bad_request`.
MAX_FRAME_BYTES = 16 * 1024


# ============================ wire framing (pure, offline-testable) =========


def encode_frame(obj: dict) -> bytes:
    """u32be length prefix + compact UTF-8 JSON. `separators=(',', ':')`
    matches JS's `JSON.stringify` default (no inserted whitespace) -- not
    load-bearing (the server's JSON.parse doesn't care about whitespace) but
    keeps frames minimal and diffs against the JS side easy to eyeball."""
    payload = json.dumps(obj, separators=(",", ":")).encode("utf-8")
    return struct.pack(">I", len(payload)) + payload


def try_decode_frame(buf: bytes) -> tuple[Optional[dict], bytes]:
    """Try to decode exactly one frame off the FRONT of `buf`. Returns
    (obj, remainder) if a complete frame is present, else (None, buf)
    UNCHANGED (so a caller can keep appending bytes and retrying). Pure --
    no I/O, no state -- the same logic backs both the real network read
    loop and the offline `--selftest`."""
    if len(buf) < 4:
        return None, buf
    (length,) = struct.unpack_from(">I", buf, 0)
    if len(buf) < 4 + length:
        return None, buf
    payload = buf[4 : 4 + length]
    obj = json.loads(payload.decode("utf-8"))
    return obj, buf[4 + length :]


def _mask_token(req: dict) -> dict:
    masked = dict(req)
    tok = masked.get("token")
    if isinstance(tok, str) and tok:
        masked["token"] = f"***({len(tok)} chars)"
    return masked


# ============================ self-test (no network, no subprocess) =========


def run_selftest() -> int:
    samples: list[dict] = [
        {"op": "worklease", "token": "t", "worker": "mac-sb", "count": 3},
        {
            "op": "workdone",
            "token": "t",
            "worker": "mac-sb",
            "quarkid": "1700000000000-0001",
            "ok": True,
            "reason": "no_savestate",
        },
        {"op": "workstats", "token": "t"},
        {
            "ok": True,
            "leased": [
                {
                    "quarkid": "1700000000000-0001",
                    "tier": 1,
                    "date": 1700000000000,
                    "duration": 4200,
                    "row": {"quarkid": "1700000000000-0001", "players": [{"name": "abc"}]},
                }
            ],
            "lease_ttl_ms": 2700000,
        },
    ]
    failures = 0

    # A known-good byte string, computed by hand, catches a struct-format
    # slip (e.g. ">I" vs "<I") that round-trip-only testing would not.
    known_obj = {"op": "status"}
    known_json = b'{"op":"status"}'  # 15 bytes, compact (separators=(',',':'))
    known_bytes = struct.pack(">I", len(known_json)) + known_json
    got_bytes = encode_frame(known_obj)
    if got_bytes != known_bytes:
        print(f"FAIL: encode_frame({known_obj!r}) = {got_bytes!r}, expected {known_bytes!r}")
        failures += 1
    else:
        print(f"PASS: encode_frame known-bytes check ({len(known_bytes)} bytes)")

    for obj in samples:
        frame = encode_frame(obj)

        # Exact whole-frame round trip.
        decoded, rest = try_decode_frame(frame)
        if decoded != obj or rest != b"":
            print(f"FAIL: round-trip mismatch for {obj!r} -> decoded={decoded!r} rest={rest!r}")
            failures += 1
            continue

        # Byte-by-byte partial delivery must never decode early, and must
        # decode to the exact original object on the last byte.
        buf = b""
        got: Optional[dict] = None
        decoded_at = -1
        for i in range(len(frame)):
            buf += frame[i : i + 1]
            d, buf = try_decode_frame(buf)
            if d is not None:
                decoded_at = i
                got = d
                break
        if decoded_at != len(frame) - 1:
            print(f"FAIL: frame for {obj!r} decoded early at byte {decoded_at}/{len(frame) - 1}")
            failures += 1
        elif got != obj:
            print(f"FAIL: incremental decode mismatch for {obj!r} -> got={got!r}")
            failures += 1
        else:
            print(f"PASS: {obj.get('op', '(response)')!r} round-trip + incremental-delivery ({len(frame)} bytes)")

    # Two frames back-to-back (pipelined) must decode independently in order
    # -- the README documents "one connection may carry many request/
    # response pairs".
    two = encode_frame(samples[0]) + encode_frame(samples[1])
    d1, rest = try_decode_frame(two)
    d2, rest2 = try_decode_frame(rest)
    if d1 != samples[0] or d2 != samples[1] or rest2 != b"":
        print(f"FAIL: back-to-back pipelined frame decode mismatch (d1={d1!r} d2={d2!r} rest2={rest2!r})")
        failures += 1
    else:
        print("PASS: back-to-back pipelined frames decode independently in order")

    if failures:
        print(f"proxy_ops --selftest: {failures} FAILURE(S)")
        return 1
    print(f"proxy_ops --selftest: {len(samples) + 2} check(s) PASS (0 network, 0 subprocess)")
    return 0


# ============================ request construction ===========================

WORKER_NAME_MAXLEN = 64  # mirrors fcade-proxy.js WORKER_NAME_RE, /^[A-Za-z0-9_.-]{1,64}$/


def load_token(args: argparse.Namespace) -> str:
    if args.token:
        return args.token
    if args.token_file:
        p = Path(args.token_file).expanduser()
        try:
            val = p.read_text(encoding="utf-8").strip()
        except OSError as exc:
            raise SystemExit(f"error: cannot read --token-file {p}: {exc}")
        if not val:
            raise SystemExit(f"error: --token-file {p} is empty")
        return val
    env = os.environ.get("FCADE_WORK_TOKEN")
    if env:
        return env
    raise SystemExit(
        "error: no token available -- pass --token/--token-file, or set the "
        "FCADE_WORK_TOKEN env var (see auto-convert/README.md 'Token configuration')"
    )


def build_request(args: argparse.Namespace) -> dict:
    if args.op == "worklease":
        req: dict[str, Any] = {"op": "worklease", "token": load_token(args), "worker": args.worker}
        if args.count is not None:
            req["count"] = args.count
        return req
    if args.op == "workdone":
        req = {
            "op": "workdone",
            "token": load_token(args),
            "worker": args.worker,
            "quarkid": args.quarkid,
            "ok": args.ok == "true",
        }
        if args.reason:
            req["reason"] = args.reason
        return req
    if args.op == "workstats":
        return {"op": "workstats", "token": load_token(args)}
    raise SystemExit(f"error: unknown op {args.op!r}")  # argparse should make this unreachable


# ============================ transport (ssh -W stdio pipe) ==================


def _read_one_frame_with_timeout(fd: int, timeout_s: float) -> dict:
    deadline = time.monotonic() + timeout_s
    buf = b""
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"timed out after {timeout_s}s waiting for a full response frame ({len(buf)} bytes so far)")
        ready, _, _ = select.select([fd], [], [], remaining)
        if not ready:
            continue
        chunk = os.read(fd, 65536)
        if not chunk:
            raise EOFError(f"ssh tunnel closed before a full response frame arrived ({len(buf)} bytes so far)")
        buf += chunk
        obj, buf = try_decode_frame(buf)
        if obj is not None:
            return obj


def send_over_ssh(
    host: str,
    port: int,
    req: dict,
    ssh_connect_timeout_s: int,
    response_timeout_s: float,
) -> dict:
    """Open `ssh -W 127.0.0.1:<port> <host>` (OpenSSH's stdio-forwarding
    primitive -- normally used for ProxyJump, repurposed here as a plain
    pipe to the proxy's loopback-only port), write one framed request, read
    exactly one framed response, then tear the SSH process down. One op per
    call, matching the rest of this repo's "one SSH round trip per script
    invocation" rail (refresh-catalog.sh, push-3sr.sh)."""
    frame = encode_frame(req)
    if len(frame) > MAX_FRAME_BYTES:
        raise SystemExit(f"error: request frame is {len(frame)} bytes, exceeds the server's {MAX_FRAME_BYTES}-byte cap -- refusing to send")

    cmd = [
        "ssh",
        "-o", "BatchMode=yes",
        "-o", f"ConnectTimeout={ssh_connect_timeout_s}",
        "-W", f"127.0.0.1:{port}",
        host,
    ]
    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert proc.stdin is not None and proc.stdout is not None
        proc.stdin.write(frame)
        proc.stdin.flush()
        try:
            proc.stdin.close()  # half-close; we have nothing more to send
        except OSError:
            pass
        try:
            return _read_one_frame_with_timeout(proc.stdout.fileno(), response_timeout_s)
        except (TimeoutError, EOFError) as exc:
            stderr_text = ""
            try:
                proc.terminate()
                proc.wait(timeout=5)
            except Exception:
                pass
            try:
                if proc.stderr is not None:
                    stderr_text = proc.stderr.read().decode("utf-8", "replace").strip()
            except Exception:
                pass
            detail = f": {stderr_text}" if stderr_text else ""
            raise RuntimeError(f"{exc} (ssh -W {host}:{port}){detail}") from exc
    finally:
        try:
            proc.terminate()
        except Exception:
            pass
        try:
            proc.wait(timeout=5)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass


# ============================ CLI ============================================


def build_parser() -> argparse.ArgumentParser:
    # Shared transport/token/print-only options, given to EVERY subparser
    # (argparse does not propagate a parent's optionals to args that follow
    # a positional subcommand, so these must be attached per-subparser via
    # `parents=`, not just declared once on the top-level parser).
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--host", default=os.environ.get("FCADE_PROXY_SSH_HOST", DEFAULT_HOST), help=f"ssh host alias to tunnel through (default: {DEFAULT_HOST})")
    common.add_argument("--port", type=int, default=int(os.environ.get("FCADE_PROXY_PORT", DEFAULT_PORT)), help=f"proxy's loopback port on the VPS (default: {DEFAULT_PORT})")
    common.add_argument("--ssh-timeout", type=int, default=DEFAULT_SSH_CONNECT_TIMEOUT_S, help="ssh ConnectTimeout, seconds")
    common.add_argument("--response-timeout", type=float, default=DEFAULT_RESPONSE_TIMEOUT_S, help="max seconds to wait for the response frame")
    common.add_argument("--token", default=None, help="shared secret directly (prefer --token-file or FCADE_WORK_TOKEN env -- avoids leaving it in shell history)")
    common.add_argument("--token-file", default=None, help="path to a file containing just the token")
    common.add_argument("--print-only", action="store_true", help="build + print the request (token masked) and exit; NEVER touches the network")

    p = argparse.ArgumentParser(
        prog="proxy_ops.py",
        description="Mac-side client for fcade-proxy's worklease/workdone/workstats ops (plan-preconvert-fleet S5).",
    )
    p.add_argument("--selftest", action="store_true", help="run the offline framing self-test and exit (no args/network needed)")

    sub = p.add_subparsers(dest="op")

    p_lease = sub.add_parser("worklease", help="claim up to --count queue items", parents=[common])
    p_lease.add_argument("--worker", required=True, help=f"worker name, matches [A-Za-z0-9_.-]{{1,{WORKER_NAME_MAXLEN}}}")
    p_lease.add_argument("--count", type=int, default=None, help="items to lease (server clamps to 1..3; default server-side is 1)")

    p_done = sub.add_parser("workdone", help="report a lease's outcome", parents=[common])
    p_done.add_argument("--worker", required=True)
    p_done.add_argument("--quarkid", required=True)
    p_done.add_argument("--ok", required=True, choices=["true", "false"], help="true: validate+integrate the staged push; false: record a failure and free the lease")
    p_done.add_argument("--reason", default=None, help="only meaningful with --ok false (e.g. no_savestate, no_games)")

    sub.add_parser("workstats", help="read-only queue/ledger snapshot", parents=[common])

    return p


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.selftest:
        return run_selftest()

    if not args.op:
        parser.error("an op is required (worklease|workdone|workstats), or pass --selftest")

    if args.op == "worklease" and args.worker is not None and len(args.worker) > WORKER_NAME_MAXLEN:
        parser.error(f"--worker must be <= {WORKER_NAME_MAXLEN} chars")
    if args.op == "workdone" and args.worker is not None and len(args.worker) > WORKER_NAME_MAXLEN:
        parser.error(f"--worker must be <= {WORKER_NAME_MAXLEN} chars")

    try:
        req = build_request(args)
    except SystemExit as exc:
        print(exc, file=sys.stderr)
        return 1

    if args.print_only:
        print(json.dumps(_mask_token(req), indent=2))
        return 0

    try:
        resp = send_over_ssh(args.host, args.port, req, args.ssh_timeout, args.response_timeout)
    except SystemExit as exc:
        print(exc, file=sys.stderr)
        return 1
    except Exception as exc:  # noqa: BLE001 -- surface any transport failure as a clean one-liner
        print(f"error: {args.op} request failed: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(resp))
    return 0 if resp.get("ok") else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
