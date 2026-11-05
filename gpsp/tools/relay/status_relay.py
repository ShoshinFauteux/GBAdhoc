"""status_relay.py -- ask the running gbadhoc-relay for its counters.

From Windows, the status port inside WSL is reached through `wsl`; inside WSL
run it with --local.  Prints one line: gbadhoc-relay ok rooms=... (or why not).
"""
import socket
import subprocess
import sys

if "--local" in sys.argv:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(2)
    s.sendto(b"status", ("127.0.0.1", 27429))
    try:
        print(s.recv(1024).decode().strip())
    except socket.timeout:
        print("gbadhoc-relay: no answer on 127.0.0.1:27429")
        sys.exit(1)
else:
    here = subprocess.run(["wsl", "-d", "Ubuntu", "--", "wslpath", "-a", __file__.replace("\\", "/")],
                          capture_output=True, text=True).stdout.strip()
    r = subprocess.run(["wsl", "-d", "Ubuntu", "--", "python3", here, "--local"], capture_output=True, text=True)
    print((r.stdout or r.stderr).strip())
    sys.exit(r.returncode)
