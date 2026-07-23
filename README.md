# linuxcnc-socketcan

`linuxcnc-socketcan` is a starting point for connecting LinuxCNC HAL to Linux
SocketCAN devices.

The project contains two non-realtime userspace HAL components:

- `lsc_socketcan`: a generic Classical CAN frame bridge
- `lsc_canopen`: an experimental XML-configured CANopen PDO mapper and master

`lsc_canopen` can keep a node's existing PDO mapping or configure a declared
mapping through expedited SDO downloads before starting the node. It also
handles NMT, heartbeat monitoring, optional SYNC production, periodic or
triggered RPDO transmission, and TPDO-to-HAL decoding.

Neither component is a hard-realtime motion-control or functional-safety
implementation.

## Project layout

- `src/lsc_socketcan.c`: generic SocketCAN-to-HAL bridge
- `src/lsc_canopen.c`: XML-configured CANopen component
- `examples/vcan.hal`: LinuxCNC HAL example using `vcan0`
- `examples/canopen.xml`: example CiA 402-style PDO mapping
- `examples/canopen.hal`: CANopen HAL loading example
- `scripts/setup-vcan.sh`: creates a virtual CAN interface for development
- `scripts/setup-can.sh`: configures a physical CAN interface
- `documentation/ARCHITECTURE.md`: design boundaries and suggested roadmap
- `documentation/CANOPEN.md`: CANopen XML and HAL reference

## Requirements

- Linux with SocketCAN support
- LinuxCNC with the userspace development headers and `halcompile`
- `linuxcnc-uspace-dev` or `linuxcnc-dev`
- Expat development headers
- `can-utils` for `candump` and `cansend`
- A supported CAN adapter, or the kernel `vcan` module for testing

On Debian-based LinuxCNC systems:

```sh
sudo apt install linuxcnc-uspace-dev libexpat1-dev can-utils
```

## Build

```sh
make
```

The local executables are created as `src/lsc_socketcan` and
`src/lsc_canopen`.

Install it into the active LinuxCNC environment with:

```sh
sudo make install
```

Do not use `sudo` for a LinuxCNC Run-In-Place environment.

## CANopen XML mapping

Start with `examples/canopen.xml`. For a virtual bus, keep
`configPdos="false"` because there is no real node to answer SDO requests:

```sh
bash scripts/setup-vcan.sh vcan0
halrun -I
```

Then load the component from the repository root:

```hal
loadusr -W lsc_canopen --config examples/canopen.xml
show pin lsc_canopen
```

For a real CANopen node, change the interface and set `configPdos="true"`
only after confirming that every declared object is PDO-mappable and that the
node supports the selected PDO communication and mapping objects.

The XML mapping is always required because it defines the HAL layout. The
flag controls whether `lsc_canopen` also writes that mapping into the node:

- `configPdos="false"`: trust the node's existing mapping
- `configPdos="true"`: configure the declared mapping through SDO

### CANopen timing in the XML

`periodUs` and `syncPeriodUs` are different clocks:

- `periodUs`: how often the `lsc_canopen` userspace loop wakes up to read CAN
  frames, update HAL pins, process RPDO sends, and check timeouts. It does not
  force a CANopen device to send PDOs.
- `syncPeriodUs`: how often `lsc_canopen` sends CANopen SYNC frames on CAN-ID
  `0x080`. Use `0` to disable SYNC production.

For example:

```xml
<socketcan interface="can0"
           periodUs="1000"
           syncPeriodUs="0"
           sdoTimeoutMs="500">
```

This wakes the driver every 1 ms, sends no SYNC frames, and waits up to 500 ms
for each SDO response during startup configuration.

PDO timing depends on the PDO configuration. A TPDO with
`transmissionType="255"` is asynchronous/event-driven, also called COV on some
devices. With `syncPeriodUs="0"`, no SYNC frames are sent, so that TPDO is
normally sent by the device on change of value or by its `eventTimerMs`, not by
`periodUs` or SYNC. `inhibitTime100us` limits how frequently the device may send
that TPDO.

For example:

```xml
<tpdo number="1"
      transmissionType="255"
      inhibitTime100us="50"
      eventTimerMs="100">
```

means an asynchronous/COV TPDO that can send on value changes, no faster than
every 5 ms, with a typical 100 ms event-timer refresh. In other words,
`eventTimerMs="100"` writes TPDO communication subindex 5 and many devices use
it to resend the TPDO about every 100 ms even when the value has not changed.
A synchronous PDO, such as `transmissionType="1"`, may act on each SYNC frame
if `syncPeriodUs` is nonzero.

See `documentation/CANOPEN.md` for the complete schema and limitations.

## Test without hardware

Create `vcan0`:

```sh
bash scripts/setup-vcan.sh vcan0
```

Start a LinuxCNC HAL session:

```sh
halrun -I
```

Then load the component:

```hal
loadusr -W lsc_socketcan --interface vcan0 --period-us 1000
show pin lsc_socketcan
```

In another terminal, send a received test frame:

```sh
cansend vcan0 321#11223344
```

Back in `halrun`, inspect the updated receive pins:

```hal
show pin lsc_socketcan.rx-*
```

To transmit from HAL, configure a frame and pulse `tx-trigger`:

```hal
setp lsc_socketcan.tx-id 0x123
setp lsc_socketcan.tx-length 2
setp lsc_socketcan.tx-data-0 0xAA
setp lsc_socketcan.tx-data-1 0x55
setp lsc_socketcan.tx-trigger true
setp lsc_socketcan.tx-trigger false
```

Observe outgoing traffic in another terminal:

```sh
candump vcan0
```

## Physical CAN interface

Configure a typical `can0` interface at 500 kbit/s with automatic bus-off
recovery:

```sh
bash scripts/setup-can.sh can0 500000 100
```

Load the HAL component:

```hal
loadusr -W lsc_socketcan --interface can0 --period-us 1000
```

## Command-line options

```text
--interface NAME   SocketCAN interface, default: can0
--period-us USEC   userspace polling period, default: 1000
--name NAME        HAL component and pin prefix, default: lsc_socketcan
--help             show usage
```

For multiple CAN buses, start the executable more than once with distinct HAL
names:

```hal
loadusr -Wn machine-can lsc_socketcan --name machine-can --interface can0
loadusr -Wn safety-can lsc_socketcan --name safety-can --interface can1
```

## HAL pins

### Control and status

- `enable` (`bit in`): opens the socket when true and closes it when false
- `connected` (`bit out`): true when the RAW socket is bound
- `bus-off` (`bit out`): set after a SocketCAN bus-off error frame
- `error-count` (`u32 out`): socket, validation, and CAN error count
- `last-error` (`s32 out`): most recent positive `errno` value
- `last-can-error` (`u32 out`): flags from the most recent CAN error frame

### Transmit

- `tx-trigger` (`bit in`): sends once on a rising edge
- `tx-id` (`u32 in`): 11-bit or 29-bit CAN identifier
- `tx-extended` (`bit in`): selects a 29-bit identifier
- `tx-rtr` (`bit in`): sends a remote-transmission-request frame
- `tx-length` (`u32 in`): payload length from 0 through 8
- `tx-data-0` through `tx-data-7` (`u32 in`): payload bytes
- `tx-count` (`u32 out`): successfully written frames

### Receive

- `rx-new` (`bit out`): toggles for every received data frame
- `rx-sequence` (`u32 out`): increments for every received data frame
- `rx-id` (`u32 out`): most recently received identifier
- `rx-extended` (`bit out`): received frame used a 29-bit identifier
- `rx-rtr` (`bit out`): received frame was an RTR frame
- `rx-length` (`u32 out`): received payload length
- `rx-data-0` through `rx-data-7` (`u32 out`): received payload bytes
- `rx-count` (`u32 out`): received data-frame count

## Where to continue

Keep `lsc_socketcan` as the transport and add device or protocol layers above
it. Good first additions are:

1. Receive filters configured from a file.
2. A frame queue instead of only exposing the latest received frame.
3. CAN FD support using `struct canfd_frame`.
4. SocketCAN Broadcast Manager support for cyclic traffic.
5. EDS/DCF/XDD parsing and verification of an existing node mapping.
6. Segmented and block SDO transfers.
7. Dedicated CiA 402 state-machine and drive profile helpers.

See `documentation/ARCHITECTURE.md` before implementing a motion-control
protocol.

## License

GPL-2.0-or-later. See `LICENSE`.
