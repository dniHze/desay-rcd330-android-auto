# RCF on the RCD 330

The unit's SVP services talk to each other with RCF, Delta V Software's C++ RPC library, over
TCP on localhost. This page describes the wire format as they speak it, and how `rcf-lite`
(`livi/native/crates/rcf-lite`), our Rust implementation, speaks it: the CarLife service
stand-in uses it to serve the stock CarLife contract and call the unit's services
([architecture.md](architecture.md)).

| | |
|---|---|
| Library | RCF 2.2.0.0 on the unit (boost 1.56); RCF 2.1.0.0 in the C++ shim; `rcf-lite` in Rust |
| Runtime version | 12 |
| Transport | TCP on `127.0.0.1`, no transport filters (no compression, no encryption) |
| Serialization | SF binary (serialization protocol 1) |
| Byte order | Little-endian everywhere |

## Frames and header values

Each message is a frame: a u32 length, then the message: a header, then the arguments of a
request or the results of a response. `rcf-lite` refuses frames over 1 MiB. Headers use their
own compact encodings:

| Type | Encoding | Examples |
|---|---|---|
| int | `00`..`fe`: one byte; anything else: `ff`, then i32 | `08` = 8; `ff ff ff ff ff` = −1 |
| bool | One byte | `00`, `01` |
| string, bytes | int length, then the bytes | `00` = `""`; `03 61 62 63` = `"abc"` |

## Requests (header version 7)

`ISVP_CarLifeService::GetCarLifeConnectStatus` (fnId 8), no arguments, 36 bytes (frame length
`24 00 00 00`):

```
01 07                 descriptor 1 (request), header version 7
13 49 53 56 50 5f 43 61 72 4c 69 66 65 53 65 72 76 69 63 65
                      service: "ISVP_CarLifeService"
00                    token: 0
00                    subinterface: "" (the service itself)
08                    fnId: 8
01                    serialization protocol: 1 (SF binary)
00 00                 one-way: no; close the connection after the call: no
0c                    runtime version: 12
00 00 00              ignore the runtime version: no; ping-back interval: 0; archive version: 0
00 00 00              user data: ""; native wide strings: no; SF pointer tracking: no
00                    out-of-band message: "" (int length, then bytes)
                      then the arguments, one SF node each
```

A one-way request gets no response. `rcf-lite` reads request headers of versions 0 to 7 and
writes version 7.

## Responses (header version 3)

```
02 03                 descriptor 2 (response), header version 3
00                    exception: no
00 00                 user data: ""; SF pointer tracking: no
00                    out-of-band message: ""
                      then the return value's node (none for void), then the node of
                      every by-reference argument, in order
```

`GetCarLifeConnectStatus` answering 0 is `02 03 00 00 00 00  00 00 01 04 00 00 00 00 02`. A
call the server cannot dispatch gets an error message instead: descriptor `00`, version `00`,
the error code as an int. `rcf-lite` answers `00 00 19` (25, no server stub) for an unknown
fnId or unreadable arguments, and `00 00 26` (38, unknown interface).

## Values: SF nodes

Every value is a node: `00` (begin), an attribute byte, the contents, `02` (end). RCF writes
attribute `00` for arguments. A reader skips what attribute bits announce: `01` an object id,
`04` a type name, `08` a label. An archive may open with metadata, `04 <runtime> <archive
version>` (plus a pointer-tracking bool from runtime 10); `rcf-lite` skips it and never writes
it. Inside a node, a count is one byte up to `7f`, else `80` and a u32.

| Value | Node | Example |
|---|---|---|
| bool, char, u8, i8 | `00 00 01 01 <byte> 02` | `00 00 01 01 01 02` = true |
| u16, i16 | `00 00 01 02 <2 bytes> 02` | `00 00 01 02 10 0e 02` = 3600 |
| int, u32, i32, float | `00 00 01 04 <4 bytes> 02` | `00 00 01 04 64 00 00 00 02` = 100 |
| i64, u64, double | `00 00 01 08 <8 bytes> 02` | |
| enum | As i32 | `00 00 01 04 02 00 00 00 02` = 2 |
| `std::string`, `std::vector<unsigned char>` | `00 00`, the length as a u32 node, the raw bytes, `02` | `"abc"`, below |
| struct | `00 00`, each field's node in the order its `serialize()` writes them, `02` | below |

Examples (a request's arguments follow its header one node each, with nothing around them):

```
00 00  00 00 01 04 03 00 00 00 02  61 62 63  02                  the string "abc"
00 00  00 00 01 01 01 02  00 00 01 01 01 02  00 00 01 01 00 02  02
       the CAN service's CarPlay info (3 x u8): day/night 1, transmission 1, RHD 0
00 00 01 04 64 00 00 00 02  00 00 01 04 c8 00 00 00 02  00 00 01 04 00 00 00 00 02
       SingleTouchInfo(100, 200, CL_TOUCH_DOWN), fnId 6: two u32, then an enum
```

## Interfaces and method ids

A call names an interface by its string and a method by its fnId: the method's position in the
interface's declaration, counted from 0. The server picks the interface by the request's
subinterface, or by its service field when the subinterface is empty. Clients put the
interface's name in the service field; published events put it in the subinterface. So a method
is the pair (interface name, fnId), for example (`ISVP_CarLifeService`, 8). The ids can differ
between firmware builds: on the SW0394 and SW0317 images the CAN service inserts a method at 30,
which moves `SendCanServiceMessage` to 31. The shim calls neither.

## Pings, subscriptions, callback connections

**Ping:** fnId −1, empty service and subinterface, no out-of-band message; the answer is the
empty response `02 03 00 00 00 00`.

**Publish and subscribe.** A publisher listens on a port of its own. The subscriber connects,
pings (as RCF's client does on a new connection), then sends a request with fnId −1 that
carries an out-of-band subscription request. After the answer it reads one-way requests on that
connection: service empty, subinterface the listener's name, fnId the listener method. The
subscription to `ISVP_CarLifeListener`, the answer, and `OnCarLifeConnectStatusChange(2, 0)`:

```
01 07 00 00 00 ff ff ff ff ff 01 00 00 0c 00 00 00 00 00 00   request header, fnId −1
19                                                            out-of-band, 25 bytes:
41 03 0c                request (magic 65), request subscription (3), runtime 12
14 49 53 56 50 5f 43 61 72 4c 69 66 65 4c 69 73 74 65 6e 65 72   "ISVP_CarLifeListener"
00                      subscriber-to-publisher ping interval: 0

02 03 00 00 00 06 42 03 0c 00 00 00      response; out-of-band: response (magic 66),
                                         subscription, runtime 12, error 0, no text, no ping
01 07 00 00 14 49 53 56 50 5f 43 61 72 4c 69 66 65 4c 69 73 74 65 6e 65 72
01 01 01 00 0c 00 00 00 00 00 00 00      fnId 1, SF, one-way, ..., no out-of-band
00 00 01 04 02 00 00 00 02               status: 2 (connecting)
00 00 01 04 00 00 00 00 02               error: 0
```

`rcf-lite`'s publisher answers error 38 for a listener it does not publish.

**Callback connection.** The source service calls its clients back on a connection the client
opened. The client calls `RegisterClient(pid)`, then sends a request with fnId −1 and the
out-of-band message `41 02 0c` (create callback connection, runtime 12); the answer carries
`42 02 0c <error>`. From then on the source service sends `SVP_SourceCallback` requests down
that connection, and the client serves them.

**Discovery** is not RCF: a service creates an empty file named after it in
`/tmp/run/service/registry/` (`svp.svc.carlife`), which clients watch with inotify.

## The services the shim uses

| Interface | Port | The shim | Methods (fnId) |
|---|---|---|---|
| `ISVP_CarLifeService` | 30407 | serves | all 19, 0–18 |
| `ISVP_CarLifeListener` | 30408 | publishes | 1 `OnCarLifeConnectStatusChange`, 7 `OnGoToDeskTop`, 8 `OnModuleStatusChange` |
| `SVP_iLMService` | 30005 | calls | 0 `WaitSurface`, 1 `ShowSurface` |
| `ISVP_AudioService` | 30203 | calls | 14 `RegisterAudio` |
| `SVP_SourceService` | 30205 | calls | 0 `RegisterClient`, 1 `RequestSource`, 2 `ReleaseSource`, 3 `ConfirmSource`, 4 `SetSourceStatus`, 8 `GetLastBGSource` |
| `SVP_SourceCallback` | callback connection to 30205 | serves | 0 `cbSourceOp` |
| `SVP_SourceListener` | 30206 | subscribes | 0 `pubSourceStatus` |
| `SVP_CanService` | 30705 | calls | 2 `GetCanVehicleSpeed`, 3 `GetCanCarplayInfo` |
| `SVP_CanListener` | 30706 | subscribes | 1 `OnCanVehicleSpeedChanged`, 2 `OnCanCarplayInfoChanged`, 22 `OnCanServiceMessage` |
| `SVP_BtService` | 30315 | calls | 5 `GetAttribute`, 8 `SetCapability`, 11 `SetECNRAudioPath`, 35 `Connect`, 37 `Disconnect`, 39 `AcceptSspRequest`, 40 `AcceptPinCodeRequest`, 44 `SetAutoAcceptConnect`, 45 `GetAutoAcceptConnect`, 52 `GetDeviceNum`, 55 `GetDevice`, 60 `GetHfpConnectAddr`, 62 `GetAvpConnectAddr` |
| `SVP_BtListener` | 30316 | subscribes | pairing and connection events (12–17, 19, 26, 29), 40 `OnCallStatusChanged` |
| `SVP_PowerService` | 30201 | calls | 3 `GetPowerState` |
| `SVP_PowerListener` | 30202 | subscribes | 0 `OnScreenOffChanged`, 1 `OnDisplayoffChanged`, 2 `OnPowerStateChanged` |

The stock CarLife service binds 30407 on `0.0.0.0`; the shim binds `127.0.0.1`. The tables also
hold `SVP_LocationService`, `SVP_LocationNotify`, `SVP_RmtService` and `SVP_RmtCallback`,
checked like the others but not used. Every interface's full table, with parameter types, is in
`livi/native/crates/livi-carlife-shim/src/idl/`.

## rcf-lite

About 1,100 lines of Rust with no dependencies beyond std; blocking, a thread per connection.
`wire` has the header encodings and frames, `msg` the headers and out-of-band messages, `sf` the
`Writer`, `Reader`, `Sf` trait and `sf_struct!`, `client` the `Client` and `Subscription`, and
`server` the `Server`, `Service` and `Publisher`.

```rust
let mut c = rcf_lite::Client::new("127.0.0.1", 30407, "ISVP_CarLifeService");
let status: i32 = rcf_lite::Reader::new(&c.call(8, &[])?).get()?;   // GetCarLifeConnectStatus
```

The client connects at the first call with RCF's default timeouts (2 s to connect, 10 s per
call), and again after the server closed the connection or a call failed. A server `Service`
reads the arguments and writes the return value, then every by-reference argument; an error
becomes error 25. A `Publisher` sends each event to every subscriber and drops one whose socket
takes nothing for 2 s, so it cannot stall the others. Not supported, and not needed by any
interface the shim uses: other serialization protocols, transport filters, object ids, pointer
tracking, polymorphic types, wide strings.

## Verifying

```sh
cd livi/native
cargo test -p rcf-lite                           # frames byte for byte; loopback
cargo test -p livi-carlife-shim --test idcheck   # method ids against the stock table
```

- **Frames:** `rcf-lite/tests/captured.rs` checks frames captured between the C++ shim (RCF
  2.1) and its peers: requests, responses, a struct, a subscription and its events.
  `tests/loopback.rs` covers reconnects, subscriptions, callback connections, stopping a server.
- **Method ids:** `aa-carlife-shim/tools/stock_ids.txt` lists every method the shim knows
  (interface, fnId, name, parameter types), read from the RCF client stubs in the stock
  libraries and services. `aa-carlife-shim/build.sh` compiles `tools/idcheck.cpp` (every C++
  stub, once), reads the ids back with `tools/rcf_ids.py` and compares them with that file; the
  Rust test does the same for `livi-carlife-shim/src/idl/`.
- **Live peers:** `rcf-lite/examples/rcf-interop.rs` talks to the C++ fake CAN service
  (`aa-carlife-shim/tools/fake_can.cpp`) and to the C++ client `aa-carlife-shim/src/cl_probe.cpp`.
  `aa-carlife-shim/test_desktop.sh` runs a shim (`SHIM=`) end to end against the fakes in
  `aa-carlife-shim/tools/`: ILM, sources, Bluetooth, CAN, power, the receiver.
