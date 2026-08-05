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

### How a guest is found to be running

Three independent signals, strongest first. Any one of them is enough.

| | signal | gives |
| --- | --- | --- |
| 1 | `pid=` in `.hms_metadata`, still a live `qvm` | state + PID |
| 2 | `/dev/qvm/<system>` exists | state |
| 3 | a `qvm` in `/proc` whose command line names this guest | state + PID |

**2 and 3 are what make a guest started outside HMS visible.** Only signal 1
existed before, and `pid=` is written by `guest_start()` — so a guest launched
from the console or a boot script was reported `stopped` while plainly running,
and the GUI offered **Start** for it. Pressing that ran a second `qvm` against a
guest that already owned its vdevs.

`<system>` is the `system` directive in the guest's `.qvmconf`; `qvm` publishes
that directory for as long as the guest lives, which is the same thing the host's
`vpctl` binds to at `/dev/qvm/guest_1/guest_to_host`.

A guest found this way is **adopted**: the PID is written back to
`.hms_metadata`, so `kill`, `restart` and OTA work on it exactly as if HMS had
started it, and the next refresh takes the cheap path.

### The address, which adoption also needs

`ip=` in `.hms_metadata` was the only place an address was ever read from, and
only `guest_start()` ever wrote one. An adopted guest therefore had a PID and no
address — correctly reported `running`, and reachable by nothing: every ssh path
checks the address first and gives up on an empty one, so the guest appeared
running and nameless with every command against it failing.

Discovery now fills it in. First answer wins:

| | source |
| --- | --- |
| 1 | `ip=` in `.hms_metadata` |
| 2 | the host-side `vp*` link bound to this guest |
| 3 | a default for the guest's type — `10.0.0.2` QNX, `10.0.1.2` Linux/Android |

Signal 2 is read off the running host rather than assumed. `vpctl` with no
assignments reports the binding it already has:

```
$ vpctl vp0
vp0: peer=/dev/qvm/guest_1/guest_to_host bind=/dev/vdevpeers/vp0 ...
```

and the peer path carries the same `system` name the guest's `.qvmconf` does.
`vp0` is guest-1 only because `.vdev_net_start.sh` binds them in that order, so
matching on the name rather than the index is what keeps a third guest from
breaking it. The guest's own address is then the host end of that link **+1** —
both images put the host at `.1` of the `/24` and the guest at `.2`. That part is
a convention, not a reading: the guest configures its address inside itself and
the host cannot see it. Renumbering the link still works — move the host to
`10.9.0.1` and the guest is looked for at `10.9.0.2`.

Whatever it resolves to is written back to `.hms_metadata`, so it is read from
the file from then on, and an address put there by hand always wins.

Signal 3 is deliberately conservative. `guest_start()` execs `qvm @<basename>`
with the guest directory as its cwd, so the directory is not on the command line
and the basename is often all there is — and two guests may ship a file of the
same name. A command line matching more than one guest is treated as naming
none: a wrong PID here is a `kill` aimed at the wrong guest. In that case the
guest still shows as `running` with PID `0`, and `kill` says why it cannot act
rather than claiming the guest is stopped.

## Guest directory layout

```
/guests/
├── guest-1/
│   ├── boot.img       ← kernel/IFS
│   ├── rootfs.img     ← root filesystem
│   └── guest.conf     ← QVM config (may carry ip=, ssh_user=, ssh_password=, ssh_key=)
└── ...
```
