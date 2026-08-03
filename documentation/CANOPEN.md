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

1. Send NMT Pre-operational when any startup SDO setup is required.
2. Optionally configure heartbeat producer object `0x1017`.
3. Disable each configured PDO through communication subindex 1.
4. Set mapping subindex 0 to zero.
5. Write every mapping entry.
6. Restore the mapping-entry count.
7. Configure transmission type, optional TPDO inhibit time, and optional TPDO
   event timer.
8. Re-enable the PDO with the configured COB-ID.
9. Write any node-level custom `<sdo>` downloads.
10. Send NMT Start when `startNode="true"`.

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
- `periodUs`: userspace polling period, values can be from 100 to 1000000 microseconds.
example: periodUs = "1000": lsc_canopen checks the CAN socket every 1 ms. It does not mean “send PDO every 1 ms”.
- `syncPeriodUs`: SYNC producer period; zero disables SYNC production.
it is specifically a parameter related to the transmissionType. If the transmissionType = "1", the syncPeriodUs will be important 
to not be 0. \
example:\
transmissionType="1"\
syncPeriodUs="10000"\
Then lsc_canopen sends SYNC every 10 ms, and the device may send/consume the PDO on each SYNC.\
For a transmissionType="255", since it is asynchronous/event-driven/COV on many devices, the syncPeriodUs could be 0.
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
  <sdo index="0x6423" subIdx="0" size="1" value="1"/>
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

Node-level `<sdo>` elements are optional expedited SDO downloads written after
PDO configuration and before NMT Start. `size` must be `1`, `2`, or `4` bytes.

## RPDO Element

```xml
<rpdo number="1"
      cobId="0x210"
      transmissionType="1"
      periodMs="10">
  ...
</rpdo>
```

- `number`: RPDO number 1 through 4
- `cobId`: Communication Object Identifier, optional 11-bit COB-ID; the predefined connection-set value is the
  default.
  $$\text{COB-ID} = \text{Base Address of the functionality} + \text{Node-ID}$$
here, we consider the node-id is 0x10, and the functionality is receive pdo (0x200 for the txpdo1, 0x300 for txp2, etc).
So: 0x200 + 0x10 = 0x210
- `transmissionType`: CANopen transmission type, default 255
- `periodMs`: automatic RPDO transmit period; zero disables periodic sending

Every RPDO also has a `send` HAL bit. A rising edge transmits one frame even
when `periodMs` is zero.

## TPDO Element

```xml
<tpdo number="1"
      cobId="0x190"
      transmissionType="1"
      inhibitTime100us="50"
      eventTimerMs="100">
  ...
</tpdo>
```

- `number`: TPDO number 1 through 4
- `cobId`: Communication Object Identifier, optional 11-bit COB-ID 
$$\text{COB-ID} = \text{Base Address of the functionality} + \text{Node-ID}$$
here, we consider the node-id is 0x10, and the functionality is transmit pdo (0x180 for the txpdo1, 0x280 for txp2, etc).
So: 0x180 + 0x10 = 0x190
- `transmissionType`: CANopen transmission type, default 255
- `inhibitTime100us`: optional TPDO communication parameter subindex 3.
  The unit is 100 microseconds. For example, `50` means 5 ms. This limits
  how frequently the node may transmit this TPDO, which is useful for
  event-driven/COV TPDOs that could otherwise send too often when inputs
  change quickly.
- `eventTimerMs`: optional TPDO communication parameter subindex 5. The unit is
  milliseconds. For event-driven/asynchronous TPDOs, many nodes use this as a
  periodic refresh timer even if no value changes.
  if you want to disable it, use 0.

For example:

```xml
<tpdo number="1"
      transmissionType="255"
      inhibitTime100us="50"
      eventTimerMs="100">
```
the device sends TPDOs when:
- the value changes, because 255 is asynchronous/event-driven/COV on many devices
- or every 100 ms, because eventTimerMs="100"
- but not more often than 5 ms, because inhibitTime100us="50" = 50 * 100 us



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
hal-value = raw-value / scale + offset
```

RPDO float encoding applies the inverse operation.

Entries are packed consecutively in XML order. The component supports
non-byte-aligned fields and uses CANopen little-endian bit ordering. A PDO may
contain at most 64 bits and 32 exported entries.

`complexEntry` can be used as a skip/pass-through field when only `bitLen` is
provided:

```xml
<complexEntry bitLen="16"/>
```

That consumes 16 payload bits and exports no HAL pin.

When named, `complexEntry` behaves like a normal numeric field and can also
export overlay bit pins from the same payload bits:

```xml
<complexEntry name="status-flags"
              index="0x2102"
              subIdx="0"
              bitLen="16"
              halType="u32"
              bit0="enable"
              bit1="fault"
              bit2="emergency"/>
```

This exports `status-flags` as a `u32` pin and also exports the named `bit`
pins. The generated bit pins do not increase the PDO payload length and are not
written as additional PDO mapping objects when `configPdos="true"`.

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
