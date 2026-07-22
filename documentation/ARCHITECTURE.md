# Architecture

## Goal

The project should provide a maintainable path from Linux SocketCAN interfaces
to stable LinuxCNC HAL pins without tying the transport layer to one device or
one CAN application protocol.

## Initial layers

### 1. SocketCAN transport

`lsc_socketcan` owns a nonblocking `CAN_RAW` socket and handles:

- interface binding
- reconnection
- Classical CAN frame transmission and reception
- SocketCAN error frames
- generic HAL frame pins and counters

It intentionally does not interpret device payloads.

### 2. Protocol layer

A future protocol layer should turn raw frames into operations such as:

- CANopen NMT state changes
- heartbeat and node guarding
- PDO production and consumption
- SDO transfers
- emergency messages
- J1939 or a proprietary device protocol

The protocol layer should not contain LinuxCNC machine semantics.

### 3. Device profiles

Device profiles should map protocol values to meaningful pins such as:

- `drive-0.controlword`
- `drive-0.statusword`
- `drive-0.position-command`
- `drive-0.position-feedback`
- `io-0.input-00`
- `io-0.output-00`

Keeping device profiles separate avoids adding device-specific switches to the
SocketCAN transport.

## Realtime boundary

The current component is a LinuxCNC non-realtime userspace process. Socket
system calls, scheduler latency, and USB CAN adapter latency are not suitable
for a hard-realtime servo loop.

For cyclic control traffic, consider this progression:

1. Use the generic bridge for discovery, diagnostics, and low-rate I/O.
2. Add receive filters and timeout monitoring.
3. Use the SocketCAN Broadcast Manager for kernel-scheduled cyclic frames.
4. Split protocol work from a small realtime HAL-facing data path if required.
5. Validate worst-case latency before commanding machine motion.

Emergency stop and safety functions must not depend only on this userspace
component.

## Suggested source layout

As the project grows:

```text
src/
  transport/
    socketcan_raw.c
    socketcan_bcm.c
  protocols/
    canopen/
      nmt.c
      pdo.c
      sdo.c
      heartbeat.c
  devices/
    generic_io.c
    cia402_drive.c
  hal/
    pins.c
  lsc_socketcan.c
```

Do not introduce this structure until the second transport or first real
protocol implementation makes the separation useful.

## Recommended next milestone

Choose one real CAN node and write down:

1. CAN identifiers it sends and receives.
2. Payload byte layout and byte order.
3. Required cyclic rates and timeouts.
4. Startup and shutdown sequence.
5. Fault and bus-off behavior.
6. HAL pins LinuxCNC needs.

Then implement that mapping as the first device profile while keeping raw
SocketCAN access reusable.
