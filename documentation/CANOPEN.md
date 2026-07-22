# CANopen XML Mapping

## Component

`lsc_canopen` is a LinuxCNC userspace HAL component built on SocketCAN. It
loads an XML file before exporting HAL pins, then manages the configured
CANopen nodes in its polling loop.

Load it with:

```hal
loadusr -W lsc_canopen --config /absolute/path/to/canopen.xml
```

Command-line overrides are available:

```text
--config FILE      required XML mapping
--interface NAME   override the XML interface
--period-us USEC   override the XML polling period
--name NAME        HAL component prefix, default lsc_canopen
```

## `configPdos`

The XML entries always define how bytes in each PDO map to HAL pins.

With `configPdos="false"`, the component does not write PDO communication or
mapping objects. The node must already use exactly the mapping declared in the
XML. The component currently does not read back and verify the existing map.

With `configPdos="true"`, the component performs this sequence on every
connection:

1. Send NMT Pre-operational.
2. Optionally configure heartbeat producer object `0x1017`.
3. Disable each configured PDO through communication subindex 1.
4. Set mapping subindex 0 to zero.
5. Write every mapping entry.
6. Restore the mapping-entry count.
7. Configure transmission type and optional TPDO event timer.
8. Re-enable the PDO with the configured COB-ID.
9. Send NMT Start when `startNode="true"`.

Only expedited SDO downloads of one, two, or four bytes are required by this
sequence and are implemented.

## Root Element

```xml
<socketcan interface="can0"
           periodUs="1000"
           syncPeriodUs="10000"
           sdoTimeoutMs="500">
  ...
</socketcan>
```

- `interface`: SocketCAN network interface
- `periodUs`: userspace polling period, 100 through 1000000 microseconds
- `syncPeriodUs`: SYNC producer period; zero disables SYNC production
- `sdoTimeoutMs`: timeout for each SDO request

SYNC uses the standard CAN-ID `0x080`.

## Node Element

```xml
<node name="axis-x"
      id="1"
      configPdos="true"
      startNode="true"
      heartbeatProducerMs="100"
      heartbeatTimeoutMs="500">
  ...
</node>
```

- `name`: HAL-safe node name
- `id`: CANopen node ID from 1 through 127
- `configPdos`: write the declared PDO map through SDO
- `startNode`: send NMT Start after configuration
- `heartbeatProducerMs`: optionally write object `0x1017:0`
- `heartbeatTimeoutMs`: mark the node offline after this heartbeat timeout;
  zero disables heartbeat supervision

The component supports up to 16 nodes.

## RPDO Element

```xml
<rpdo number="1"
      cobId="0x201"
      transmissionType="1"
      periodMs="10">
  ...
</rpdo>
```

- `number`: RPDO number 1 through 4
- `cobId`: optional 11-bit COB-ID; the predefined connection-set value is the
  default
- `transmissionType`: CANopen transmission type, default 255
- `periodMs`: automatic RPDO transmit period; zero disables periodic sending

Every RPDO also has a `send` HAL bit. A rising edge transmits one frame even
when `periodMs` is zero.

## TPDO Element

```xml
<tpdo number="1"
      cobId="0x181"
      transmissionType="1"
      eventTimerMs="100">
  ...
</tpdo>
```

- `number`: TPDO number 1 through 4
- `cobId`: optional 11-bit COB-ID
- `transmissionType`: CANopen transmission type, default 255
- `eventTimerMs`: optional TPDO communication parameter subindex 5

## PDO Entries

```xml
<pdoEntry name="actual-position"
          index="0x6064"
          subIdx="0"
          bitLen="32"
          halType="s32"/>
```

Available attributes:

- `name`: HAL pin name segment
- `index`: Object Dictionary index
- `subIdx`: Object Dictionary subindex
- `bitLen`: mapped length from 1 through 32 bits
- `halType`: `bit`, `u32`, `s32`, or `float`
- `signed`: raw signed interpretation for a float entry
- `scale`: raw-to-HAL multiplier for a float entry
- `offset`: raw-to-HAL offset for a float entry

Float decoding uses:

```text
hal-value = raw-value * scale + offset
```

RPDO float encoding applies the inverse operation.

Entries are packed consecutively in XML order. The component supports
non-byte-aligned fields and uses CANopen little-endian bit ordering. A PDO may
contain at most 64 bits and 16 entries.

## HAL Pins

Global pins:

```text
lsc_canopen.connected
lsc_canopen.config-ok
lsc_canopen.bus-off
lsc_canopen.error-count
lsc_canopen.last-error
lsc_canopen.sync-count
```

Node status pins:

```text
lsc_canopen.axis-x.enable
lsc_canopen.axis-x.online
lsc_canopen.axis-x.operational
lsc_canopen.axis-x.config-ok
lsc_canopen.axis-x.nmt-state
lsc_canopen.axis-x.heartbeat-age-ms
lsc_canopen.axis-x.sdo-abort
```

RPDO pins are HAL inputs:

```text
lsc_canopen.axis-x.rpdo-1.controlword
lsc_canopen.axis-x.rpdo-1.target-position
lsc_canopen.axis-x.rpdo-1.send
lsc_canopen.axis-x.rpdo-1.tx-count
lsc_canopen.axis-x.rpdo-1.last-error
```

TPDO values are HAL outputs:

```text
lsc_canopen.axis-x.tpdo-1.statusword
lsc_canopen.axis-x.tpdo-1.actual-position
lsc_canopen.axis-x.tpdo-1.rx-count
lsc_canopen.axis-x.tpdo-1.last-error
```

`sdo-abort` contains the last CANopen SDO abort code. `last-error` pins contain
positive Linux `errno` values.

## Virtual CAN Test

The example uses `configPdos="false"`, so it can run without an SDO server:

```sh
bash scripts/setup-vcan.sh vcan0
halrun -f examples/canopen.hal
```

Inject the example TPDO:

```sh
cansend vcan0 181#34127856341208
```

Observe:

```sh
halcmd show pin lsc_canopen.axis-x.tpdo-1
```

The periodic example RPDO can be observed with:

```sh
candump vcan0,201:7FF
```

## Current Limits

- Classical CAN only; no CAN FD
- Standard 11-bit CANopen COB-IDs only
- Four RPDOs and four TPDOs per node
- Sixteen mapped entries per PDO
- Explicit XML mapping is required
- No EDS, DCF, XDD, or XDC import yet
- No SDO upload or existing-map verification yet
- No segmented or block SDO transfer
- No LSS master
- No dedicated CiA 402 state-machine helper
- Userspace scheduling is not hard realtime

Verify the mapping with the device manual before enabling machine motion.
Functional safety and emergency stop functions must remain independent of
this component.
