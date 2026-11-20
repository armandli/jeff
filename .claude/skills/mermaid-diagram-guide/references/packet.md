# Packet Diagram

**Keyword**: `packet-beta`
**Use for**: Network packet structure, protocol headers, bit-field documentation

Note: Beta feature (v11+).

## Basic Syntax

Each line defines a field with its bit range and a label:

```
packet-beta
    0-7: "Field Name"
    8-15: "Another Field"
```

Bit positions are zero-indexed from left (most significant).

## Auto-Increment Syntax (v11.7.0+)

Use `+N` instead of explicit ranges — positions auto-increment from the previous field's end:

```
packet-beta
    +8: "Source Port"
    +8: "Destination Port"
    +16: "Length"
    +16: "Checksum"
```

This is easier to maintain — inserting a field doesn't require renumbering everything.

## Mixed Syntax

You can mix explicit ranges and `+N`:
```
packet-beta
    0-3: "Version"
    +4: "Header Length"
    8-15: "DSCP"
    +2: "ECN"
```

## Multi-Row Layout

Fields automatically wrap to the next row based on configured row width (default 32 bits):

```
packet-beta
    0-3: "Version"
    4-7: "IHL"
    8-15: "TOS"
    16-31: "Total Length"
    32-47: "Identification"
    48-50: "Flags"
    51-63: "Fragment Offset"
```

## Configuration

```
---
config:
  packet:
    bitsPerRow: 32        # bits per row (default 32)
    showBits: true        # show bit numbers (default true)
    paddingX: 5
    paddingY: 5
---
packet-beta
    ...
```

## Examples

**UDP header:**
```
packet-beta
    0-15: "Source Port"
    16-31: "Destination Port"
    32-47: "Length"
    48-63: "Checksum"
    64-95: "Data (variable)"
```

**TCP header (abbreviated):**
```
packet-beta
    0-15: "Source Port"
    16-31: "Destination Port"
    32-63: "Sequence Number"
    64-95: "Acknowledgment Number"
    96-99: "Data Offset"
    100-105: "Reserved"
    106: "URG"
    107: "ACK"
    108: "PSH"
    109: "RST"
    110: "SYN"
    111: "FIN"
    112-127: "Window Size"
```

**IPv4 header using auto-increment:**
```
packet-beta
    +4: "Version"
    +4: "IHL"
    +6: "DSCP"
    +2: "ECN"
    +16: "Total Length"
    +16: "Identification"
    +3: "Flags"
    +13: "Fragment Offset"
    +8: "TTL"
    +8: "Protocol"
    +16: "Header Checksum"
    +32: "Source IP"
    +32: "Destination IP"
```

**Custom protocol frame:**
```
packet-beta
    0-7: "Magic (0xAB)"
    8-15: "Version"
    16-31: "Payload Length"
    32-63: "Timestamp"
    64-95: "Sequence Number"
    96-127: "Checksum"
```
