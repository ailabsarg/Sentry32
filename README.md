# Sentry32
A robust ESP32-based LAN sentinel with a built-in web interface, ARP-based device discovery,
persistent device tracking, and Wake-on-LAN (WOL) control.




Designed for **maximum reliability** in real-world networks  
(Windows/Linux firewalls, routers blocking ICMP, timing issues, etc.)

---

## ✨ Features

- 🌐 **Embedded web interface**
  - View discovered devices
  - Trigger Wake-on-LAN packets
  - Auto & Manual network scan
  - Forget WiFi & reset configuration

- 📡 **Deterministic network discovery**
  - Scans all 254 hosts in the local subnet
  - Uses **Ping as a trigger**, not a gate
  - Always performs ARP lookup (firewall-proof)
  - Retries ARP to mitigate timing issues
  - Never removes previously discovered devices

- 🔐 **Web interface**
  - HTTP Basic Authentication
  - Credentials configurable via captive portal
  - No hardcoded real secrets

- ⚙️ **First-boot configuration**
  - WiFiManager captive portal
  - Configure:
    - WiFi credentials
    - Web UI username/password
    - Optional Worker ID (heartbeat)

- 💾 **Persistent storage**
  - Devices stored in ESP32 NVS (flash)
  - Credentials stored locally
  - Survives reboots and power loss

- ❤️ **Heartbeat reporting**
  - Periodic HTTPS POST with device status
  - Configurable worker ID
  - Automatic retry on failure

- 🔄 **Background scanning**
  - Periodic automatic scans
  - Manual scan trigger via web UI
  - Runs in a dedicated FreeRTOS task

- 💡 **Status LED**
  - Fast blink: AP mode / disconnected
  - Slow blink: connected & operational

- 🛡️ **Watchdog protected**
  - Designed for unattended, long-running operation

---

## 🧠 Network Discovery Philosophy

Sentry32 prioritizes **robustness over speed**.

- All 254 IPs are pinged (best-effort)
- Ping failures **do NOT block discovery**
- ARP is always queried
- Windows hosts with ICMP blocked are still detected
- Devices are only ever **added**, never removed automatically

This approach matches real-world LAN behavior far better than ICMP-only scanners.

---

## 🛠 Hardware

- ESP32 (tested on common ESP32 Dev Modules)
- Optional status LED (GPIO2 by default)

---

## 📦 Required Libraries (Known-Good Versions)

⚠️ **Important:**  
Newer versions of some libraries have been observed to break ARP or networking behavior.

| Library | Version |
|------|------|
| WiFiManager | **2.0.17** |
| ESPping | **1.0.5** |
| ArduinoJson | **7.4.x** |
| HTTPClient | Arduino standard |

---

## 🚀 Getting Started

1. Flash the firmware to your ESP32  
2. Power it on  
3. Connect to the WiFi access point:

```
SSID: ControlPC_AP
Password: changeme1234
```

4. Open in your browser:

```
http://192.168.4.1
```

5. Configure:
- WiFi credentials
- Web UI username/password
- Worker ID (for heartbeat) (optional)

6. Save and reboot

---

## 🌐 Web Interface

Once connected to your LAN, access Sentry32 at:

```
http://<esp32-ip-address>
```

### Available endpoints

| Endpoint | Description |
|-------|------------|
| `/` | Main web UI |
| `/scan` | Trigger manual scan |
| `/devices` | View discovered devices |
| `/wake?mac=XX:XX:XX:XX:XX:XX` | Send Wake-on-LAN packet |
| `/clear` | Clear stored devices |
| `/forget` | Forget WiFi & settings |

---

## 🔌 Integration & Automation

Sentry32 is designed to integrate easily with external systems and automation workflows.

Although it maintains a persistent list of discovered devices, **Wake-on-LAN is not limited to discovered hosts**.

A magic packet can be sent to **any valid MAC address**, even if the device has never been discovered:

```
/wake?mac=AA:BB:CC:DD:EE:FF
```

All endpoints are protected by **HTTP Basic Authentication**.

---

## ❤️ Heartbeat / Worker Registration

Sentry32 can periodically (5 min) send a JSON heartbeat to a configurable HTTPS endpoint:

```json
{
  "hostname": "esp32-XXXXXX",
  "ip": "192.168.1.50",
  "worker_id": "optional-custom-id"
}
```

This can be integrated with monitoring or inventory systems.

---

## 📜 License

**Apache License 2.0**

You are free to:
- Use commercially
- Modify
- Distribute

With explicit patent protection and clear attribution terms.

---

## 🤝 Contributions

Issues and pull requests are welcome.

---

## ⭐ Final Notes

Sentry32 is designed to run for months without babysitting.

If you need a reliable ESP32-based LAN sentinel with:
- Deterministic discovery
- Persistent memory
- Web-based control
- Wake-on-LAN capabilities

**Sentry32 is built for exactly that purpose.**

## 🔐 Transport Security (HTTP vs HTTPS)

Sentry32 exposes its web interface over **plain HTTP**.

- All endpoints are protected by **HTTP Basic Authentication**
- Credentials are **not encrypted at the transport layer**
- Traffic can be sniffed by:
  - Other users on the **same local network**
  - Network operators (e.g. ISP) *if* the device is exposed to the Internet

This is a **deliberate design choice**.

### Design assumptions

Sentry32 is designed to run:

- Inside a **trusted LAN**
- Behind **NAT, firewall, or VPN**
- Prioritizing:
  - Simplicity
  - Deterministic behavior
  - Long-term stability on ESP32 hardware

TLS is intentionally avoided to reduce complexity and memory usage.

### ⚠️ Internet exposure warning

Sentry32 is **not designed to be exposed directly to the public Internet**.

If exposed and credentials are compromised, an attacker could:
- Trigger Wake-on-LAN packets
- Force device reconfiguration
- Cause denial-of-service on this device

### Recommendations

- Do not expose port 80 publicly
- Deploy on a trusted network
- For remote access, use:
  - VPN
  - SSH tunnel
  - Reverse proxy with TLS
- Use **strong, unique credentials**


