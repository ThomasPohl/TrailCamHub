# Dev container with USB boards

`devcontainer.json` in this directory is a second dev container configuration.
It bind-mounts the host's `/dev` at `/dev/host`, and `post-start.sh` links the
serial devices to their usual names (`/dev/ttyACM0` to `9`, `/dev/ttyUSB0` to
`9`, `/dev/serial`), so `idf.py -p /dev/ttyACM0 flash monitor` works from
inside the container. These links dangle until a board with that number is
plugged in. Choose the configuration with **Dev Containers: Rebuild and
Reopen in Container**. The default configuration in `.devcontainer/` has no
device access and is the one Codespaces uses. Both build the same image
(`.devcontainer/Dockerfile`) and share the cache volumes.

## What the container may access

The container does not run with `--privileged`. It sees all nodes of the
host's `/dev` under `/dev/host`. Two device cgroup rules add the major numbers
of the serial ports to the devices Docker allows by default: 166 (`ttyACM`:
USB-Serial-JTAG, CH343) and 188 (`ttyUSB`: CH34x and other USB-UART bridges).

The host's `/dev` must not be mounted on `/dev` itself. The container would
then lose its own `/dev/pts`, and with rootless Docker every `docker exec -t`
fails, including the lifecycle commands of the dev container: `reopen ptmx to
get new pty pair: reopen fd 9: permission denied` (runc opens `/dev/pts/ptmx`,
which has mode 000 on a typical host).

The configuration is meant for **rootless Docker**: there the container's
`root` is the host user and has no more rights than that user. The device
cgroup rules do not restrict anything in this case; the host user's
permissions decide (see the host setup below).

With **rootful Docker** the container's `root` is the host's `root`. The
device cgroup rules then keep disks closed, but the container can open every
serial device of the host, whatever its owner and mode. Claude Code runs in
this container; keep that in mind before you use the configuration with
rootful Docker.

The bind mount of `/dev` is recursive, so the host's terminals
(`/dev/host/pts`, allowed by Docker's defaults, major 136), shared memory
(`/dev/host/shm`, a tmpfs, which no device rule covers) and POSIX message
queues (`/dev/host/mqueue`) would be visible as well. Empty tmpfs mounts hide
all three (`mounts` in `devcontainer.json`). Check after a rebuild, with
rootless and with rootful Docker:

```bash
ls -A /dev/host/pts /dev/host/shm /dev/host/mqueue   # all empty
esptool.py --port /dev/ttyACM0 read_mac              # serial still works
```

## Host setup (Linux)

The container can only open a device that the host user can open. With
rootless Docker the container's `root` is the host user, and the group
`dialout` of the host is not mapped into the container, so being in
`dialout` on the host is not enough. Give the boards' serial devices to the
user with a udev rule, for example `/etc/udev/rules.d/99-trailcamhub.rules`:

```
# Espressif USB-Serial-JTAG (ESP32-C5, ESP32-S3, ESP32-P4)
SUBSYSTEM=="tty", ATTRS{idVendor}=="303a", MODE="0666"
# WCH CH343/CH340 USB-UART bridges (e.g. on the ESP32-P4-NANO)
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", MODE="0666"
```

`MODE="0666"` lets every local user open these devices. On a desktop where
only the logged-in user should have them, use `TAG+="uaccess"` instead of
`MODE="0666"`; this does not work for a user who is only logged in over SSH.

Then reload the rules and plug the boards in again:

```bash
sudo udevadm control --reload-rules && sudo udevadm trigger
ls -l /dev/serial/by-id/
```

## In the container

```bash
ls -l /dev/serial/by-id/
. "$IDF_PATH/export.sh"
idf.py -B build-c5 -p /dev/ttyACM0 flash monitor
```

The ESP32-C5 and ESP32-S3 dongles enumerate again after a reset into the
bootloader. The bind mount of `/dev` shows the new device node, but its
number can change (`ttyACM0` → `ttyACM1`); the names in `/dev/serial/by-id/`
stay the same.
