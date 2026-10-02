#!/usr/bin/python3 -I
"""Synthetic FreeRDP process adapter. All files/values belong to a test sandbox."""
import base64
import json
import os
from pathlib import Path
import select
import signal
import sys
import termios
import time

root = Path(__file__).resolve().parent
settings = json.loads((root / "settings.json").read_text())
buildconfig = sys.argv[1:] == ["/buildconfig"]
version = sys.argv[1:] == ["/version"]


def report(kind, **values):
    with (root / "events.jsonl").open("a") as output:
        output.write(json.dumps(dict(kind=kind, **values)) + "\n")


keys = ["WLOG_LEVEL", "WLOG_APPENDER", "WLOG_FIXTURE_SECRET",
        "FREERDP_ASKPASS", "SSLKEYLOGFILE", "SESSION_FIXTURE_KEEP"]
report("version" if version else "buildconfig" if buildconfig else "transport", pid=os.getpid(), argv=sys.argv[1:],
       environment={key: os.environ.get(key) for key in keys})
if version:
    if settings.get("version_crash"):
        os.kill(os.getpid(), signal.SIGKILL)
    time.sleep(settings.get("version_delay", 0))
    sys.stdout.write(settings.get("version", "This is FreeRDP version 3.32.1\n"))
    sys.stdout.flush()
    sys.exit(settings.get("version_exit", 0))

if buildconfig:
    if settings.get("break_transport_exec"):
        Path(__file__).write_text("#!" + str(root / "missing-interpreter") + "\n")
    if settings.get("buildconfig_crash"):
        os.kill(os.getpid(), signal.SIGKILL)
    time.sleep(settings.get("buildconfig_delay", 0))
    sys.stderr.write(settings.get("buildconfig_stderr", ""))
    sys.stderr.flush()
    sys.stdout.write(settings.get("buildconfig", "Build configuration: WITH_SSO_MIB=OFF WITH_AAD=ON\n"))
    sys.stdout.flush()
    sys.exit(settings.get("buildconfig_exit", 0))

if settings.get("ignore_term"):
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
reading_input = not settings.get("pause_input", False)
flags = termios.tcgetattr(0)
os.set_blocking(0, False)
with (root / "commands.jsonl").open() as commands:
    # A new transport must not replay commands sent to its predecessor.
    commands.seek(0, 2)
    report("terminal", private=os.isatty(0) and os.isatty(1) and not os.isatty(2),
           echo=bool(flags[3] & (termios.ECHO | termios.ECHONL)),
           canonical=bool(flags[3] & termios.ICANON))
    pending = ""
    while True:
        chunk = commands.readline()
        if chunk:
            pending += chunk
            if not pending.endswith("\n"):
                report("partial-command", length=len(pending))
            else:
                command = json.loads(pending)
                pending = ""
                if "exit" in command:
                    report("exit", code=command["exit"])
                    sys.exit(command["exit"])
                if command.get("resume_input"):
                    reading_input = True
                payload = base64.b64decode(command.get("bytes", ""))
                destination = 2 if command.get("stderr") else 1
                while payload:
                    try:
                        count = os.write(destination, payload)
                        payload = payload[count:]
                    except BlockingIOError:
                        select.select([], [destination], [], 1)
                if "sequence" in command:
                    report("output", sequence=command["sequence"])
                if "exit_after_output" in command:
                    report("exit", code=command["exit_after_output"])
                    sys.exit(command["exit_after_output"])
        readable, _, _ = select.select([0] if reading_input else [], [], [], 0.01)
        if readable:
            data = os.read(0, 8192)
            if not data:
                sys.exit(0)
            report("input", bytes=base64.b64encode(data).decode("ascii"))
