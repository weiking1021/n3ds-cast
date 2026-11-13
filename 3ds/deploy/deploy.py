"""Uploads a build to a console over FTP (a homebrew FTP server such as ftpd on the 3DS).

    python deploy.py DIRECTORY

DIRECTORY holds n3ds-cast.3dsx (uploaded to /3ds) and/or n3ds-cast.cia (uploaded to /cias). The console's FTP server
is flaky, so every step uses a fresh connection and is retried, a file is uploaded under a temporary name, read back
and compared, and only then moved into place: an interrupted run never leaves a half-written file behind.

Environment: N3DS_FTP_HOST (default 3ds-ftpd.local), N3DS_FTP_PORT (default 5000).
Run it through Docker, see the root README.
"""

import ftplib
import hashlib
import io
import os
import pathlib
import sys
import time

TARGETS = {"n3ds-cast.3dsx": "/3ds", "n3ds-cast.cia": "/cias"}
CONNECT_TIMEOUT = 8
CHUNK = 8192


class Console(ftplib.FTP):
    """FTP client for a server that answers late: every command is followed by a NOOP whose reply marks its end."""

    def __init__(self):
        super().__init__()
        self.noop_pending = False

    def putcmd(self, line):
        if "\r" in line or "\n" in line:
            raise ValueError("Invalid FTP command")
        if line == "QUIT" or line.startswith(("RNFR ", "RNTO ")):
            return super().putcmd(line)
        self.noop_pending = not line.startswith(("STOR ", "RETR ", "LIST", "NLST"))
        self.sock.sendall((line + "\r\nNOOP\r\n").encode("utf-8"))

    def getresp(self):
        pending, self.noop_pending = self.noop_pending, False
        response = super().getresp()
        if pending:
            noop = super().getresp()
            if not noop.startswith("200"):
                raise RuntimeError("Unexpected NOOP reply: " + noop)
        return response

    def finish(self, response):
        if response.startswith("200"):
            response = self.getresp()
        if not response.startswith("226"):
            raise RuntimeError("Transfer incomplete: " + response)

    def shutdown(self):
        try:
            self.quit()
        except Exception:
            self.close()


def connect():
    host = os.environ.get("N3DS_FTP_HOST", "3ds-ftpd.local")
    port = int(os.environ.get("N3DS_FTP_PORT", "5000"))
    console = Console()
    console.connect(host, port, timeout=CONNECT_TIMEOUT)
    console.login()
    return console


def run(action, tries=6):
    """Runs action(connection) on a fresh connection, retrying when the server drops it."""
    for attempt in range(tries):
        console = None
        try:
            console = connect()
            value = action(console)
            console.shutdown()
            return value
        except Exception as error:
            print("  retry", attempt + 1, type(error).__name__, flush=True)
            time.sleep(2)
        finally:
            if console:
                try:
                    console.close()
                except Exception:
                    pass
    raise RuntimeError("FTP kept failing")


def read(console, path):
    buffer = io.BytesIO()
    console.finish(console.retrbinary("RETR " + path, buffer.write, CHUNK))
    return buffer.getvalue()


def write(console, path, payload):
    console.finish(console.storbinary("STOR " + path, io.BytesIO(payload), CHUNK))


def names(directory):
    lines = []

    def listing(console):
        console.finish(console.retrlines("LIST " + directory, lines.append))

    run(listing)
    return {line.split(" ")[-1] for line in lines}


def remove_if_present(directory, name):
    if name in names(directory):
        try:
            run(lambda console: console.delete(directory + "/" + name), tries=3)
        except Exception as error:
            print("  could not delete", name, error)


def upload(path, directory):
    payload = path.read_bytes()
    digest = hashlib.sha256(payload).hexdigest().upper()
    final = directory + "/" + path.name
    temporary = directory + "/." + path.name + ".tmp"
    previous = directory + "/." + path.name + ".old"
    print(f"{path.name}: {len(payload)} bytes, SHA-256 {digest[:8]}", flush=True)

    def try_mkdir(console):
        try:
            console.mkd(directory)
        except Exception:
            pass  # exists already

    run(try_mkdir)
    # Leftovers of an interrupted run: FAT cannot rename onto an existing name.
    remove_if_present(directory, temporary.split("/")[-1])
    remove_if_present(directory, previous.split("/")[-1])

    run(lambda console: write(console, temporary, payload))
    if run(lambda console: read(console, temporary)) != payload:
        raise RuntimeError("temporary upload does not match")
    print("  temporary upload verified", flush=True)

    try:
        console = connect()
        try:
            if path.name in names(directory):
                console.rename(final, previous)
            console.rename(temporary, final)
            console.shutdown()
        finally:
            console.close()
    except Exception as error:
        print("  rename step failed:", type(error).__name__, error, flush=True)

    installed = run(lambda console: read(console, final)) if path.name in names(directory) else b""
    for _ in range(6):
        if installed == payload:
            break
        print("  installed file not verified; writing it directly", flush=True)
        try:
            run(lambda console: write(console, final, payload), tries=2)
            installed = run(lambda console: read(console, final))
        except Exception as error:
            print("  direct write failed", type(error).__name__, flush=True)
    if installed != payload:
        raise SystemExit(f"FAILED: {final} on the console is NOT the expected build")
    remove_if_present(directory, temporary.split("/")[-1])
    remove_if_present(directory, previous.split("/")[-1])
    print(f"  installed {final}", flush=True)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    folder = pathlib.Path(sys.argv[1])
    found = [(folder / name, directory) for name, directory in TARGETS.items() if (folder / name).is_file()]
    if not found:
        raise SystemExit(f"nothing to upload: no {' or '.join(TARGETS)} in {folder}")
    for path, directory in found:
        upload(path, directory)
    print("SUCCESS", flush=True)


if __name__ == "__main__":
    main()
