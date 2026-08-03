# Hypervisor Management System (HMS)

Management service for QNX Hypervisor guests, controlled over **MQTT**
(commands) with **SCP** for file transfer (OTA updates). The GUI client
lives in `ota_update_gui/` at the repo root.

```
┌─────────────────────┐   MQTT commands (139.185.38.211:1883)   ┌──────────────────────┐
│   hms (QNX host)    │  ─────────────────────────────────────>  │   ota_update_gui    │
│                     │  <── topics: hms/cmd, hms/status ──────  │      (Laptop)       │
│   /guests/* qvm     │                                           └──────────────────────┘
└─────────────────────┘
```

## Build

```sh
make
```

Output: `build/hms` (aarch64 QNX binary, links the bundled libmosquitto
from `../mqtt_libs/install_qnx`).

## Deploy on host

```sh
make deploy HOST=root@192.168.2.2
```

This copies `build/hms` to `/bin/hms`, installs the bundled
`libmosquitto.so.2.1.2` into `/lib` (with the `libmosquitto.so.1`
symlink HMS needs) and installs `/etc/hms.conf`.

Or manually:

```sh
scp build/hms root@192.168.2.2:/bin/hms
scp ../mqtt_libs/install_qnx/lib/libmosquitto.so.2.1.2 root@192.168.2.2:/lib/
ssh root@192.168.2.2 'ln -sf libmosquitto.so.2.1.2 /lib/libmosquitto.so.1'
scp config/hms.conf root@192.168.2.2:/etc/hms.conf
```

Verify the library resolves on the host:

```sh
ldd /bin/hms     # must NOT report libmosquitto.so.1 as missing
```

## Run

```sh
hms
```

No menu: HMS connects to the MQTT broker and serves commands until
killed. It also publishes the guest list every 10 s so the GUI stays
fresh.

## MQTT protocol

| Topic | Direction | Format | Description |
|-------|-----------|--------|-------------|
| `hms/cmd` | GUI → HMS | plain text | Commands (below) |
| `hms/status` | HMS → GUI | JSON | Responses / notifications |

Credentials: user `mqttuser`, password `123456` (see `mqtt/mqtt_client.h`).

### Commands (payload on `hms/cmd`)

| Command | Description |
|---------|-------------|
| `list` | Publish the guest list |
| `start <guest> [ip]` | Start a guest, optionally set/persist its IP |
| `kill <guest>` | Kill a running guest |
| `info <guest>` | Full guest details (paths, SSH settings, PID...) |
| `exec <guest> <cmd>` | Run a command inside the guest via SSH |
| `ota <guest> <remote_path>` | Apply an update package (below) |
| `ping` | Health check → `{"state":"pong"}` |

### Status messages (payload on `hms/status`)

- `{"state":"guest_list","guests":[{id,type,state,pid,ip},...]}`
- `{"state":"guest_info","guest":{...full info...}}`
- `{"state":"result","cmd":"...","guest":"...","success":bool,"msg":"..."}`
- `{"state":"exec_result","guest":"...","output":"..."}`
- `{"state":"ota_progress","guest":"...","stage":"download|extract|apply|restart|failed","progress":0-100,"msg":"..."}`
- `{"state":"ota_result","guest":"...","success":bool,"msg":"..."}`
- `{"state":"pong"}`

## OTA update flow

```
Laptop ──scp──> server (maxmaster@139.185.38.211:/home/maxmaster/uploads/)
Laptop ──mqtt──> hms/cmd: "ota <guest> /home/maxmaster/uploads/<pkg>"
QNX host ──scp──> server (pull package)
QNX host: kill guest → extract/apply into /guests/<guest>/ → restart guest
```

Packages can be a `.tar.gz` / `.tar` archive (extracted into the guest
directory; a single top-level folder inside the archive is treated as the
payload root) or a plain file (copied as-is into the guest directory).

The SSH key used for the server SCP pull is configured in `/etc/hms.conf`:

```
ota_server=maxmaster@139.185.38.211     # user@host of the jump server
ota_server_key=/.ssh/id_ed25519         # private key ON THE QNX HOST authorized on the server
```

Deploy the host's public key to the server (`~/.ssh/authorized_keys`)
so HMS can SCP the package down.

## Architecture

```
HMS main (MQTT dispatch) → Guest Manager → QVM / kill
                         → SSH Client    → ssh user@guest command
                         → Discoverer    → /proc + /guests/
                         → OTA module    → scp pull, extract, restart
```

Guest type is detected automatically from guest.conf:
- `load` → QNX
- `kernel=` → Linux
- `bootimg=` → Android

## Guest directory layout

```
/guests/
├── guest-1/
│   ├── boot.img       ← kernel/IFS
│   ├── rootfs.img     ← root filesystem
│   └── guest.conf     ← QVM config (may carry ip=, ssh_user=, ssh_password=, ssh_key=)
└── ...
```
