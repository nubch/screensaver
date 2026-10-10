# Xbox TCP Screensaver

Original Xbox nxdk homebrew: a **real TCP listener running on the Xbox**, displaying incoming text as individually colored, rotating, DVD-style bouncing labels.

## Install

1. Download the successful GitHub Actions build artifact (`screensaver-xbe`).
2. Copy `default.xbe` by FTP to `E:\Apps\Screensaver\default.xbe`.
3. Start it in EvolutionX/UnleashX.
4. The Xbox shows its own IPv4 address and **TCP port 4242** at the top. Both devices must be on the same local network.

## Send a message from Ubuntu

```bash
sudo apt install netcat-openbsd
printf 'HALLO XBOX\n' | nc -N 192.168.1.123 4242
printf 'DER BORZOI IST ZURUECK\n' | nc -N 192.168.1.123 4242
```

Replace the IP with the **IP displayed on your console**. Every line = one animation. Leave netcat interactive and type several messages, pressing Enter each time.

For any machine with Python 3 (no dependencies):

```python
import socket
with socket.create_connection(("192.168.1.123",4242), timeout=5) as s:
    s.sendall(b"HALLO XBOX\n")
```

## Behaviors

- 14 simultaneous floating messages (oldest reused after 14)
- Random vivid color, direction, speed, scale, starting rotation and angular velocity **for each message**
- Physics uses the rotated text's bounding rectangle for accurate edge rebounds
- Real-time displayed IPv4, client count, message count and server status
- DHCP via nxdk lwIP, TCP/IP socket bound to `0.0.0.0:4242`
- Line endings LF and CRLF supported; receiving EOF also commits a partial line
- ASCII rendering (non-ASCII bytes are displayed as `?`); messages truncated at 46 bytes
- TCP is plain text without authentication. **LAN only**: never forward this port to the internet
- No background services, scripts or PC needed after startup; everything runs on your Xbox

## Developer notes

Uses `NXDK_SDL=y` and `NXDK_NET=y`, with the SDK's standard `nxNetInit(NULL)` plus `lwip_socket`, `lwip_select`, `lwip_recv` APIs. The external `g_pnetif` provides the DHCP IPv4 address. XBE is built in GitHub Actions against current nxdk.

Performance and DHCP behavior require validation on actual console hardware.
