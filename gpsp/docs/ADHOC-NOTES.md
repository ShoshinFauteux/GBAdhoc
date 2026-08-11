# ADHOC-NOTES — pspsdk header audit for the netdrv ad-hoc transport

**Source of truth:** pspsdk headers from the current pspdev/pspdev toolchain (GCC 15.2),
audited 2026-07-31. Every signature/constant below carries a `header:line` citation.
Anything the headers do **not** state is listed in §10 ("Needs cross-check") — do not
treat those items as known until verified against PPSSPP source / known-good homebrew
(that cross-check is a separate task).

Headers audited: `pspnet.h`, `pspnet_adhoc.h`, `pspnet_adhocctl.h`,
`pspnet_adhocmatching.h`, `psputility_netmodules.h`, `psputility.h`, `pspwlan.h`,
`psppower.h`, `pspthreadman.h`, `psptypes.h`, `pspkerneltypes.h`.

---

## 1. Module load, stack init, teardown

### 1.1 Module load (user mode, fw 2.00+)

```c
int sceUtilityLoadNetModule(int module);     // psputility_netmodules.h:40
int sceUtilityUnloadNetModule(int module);   // psputility_netmodules.h:49
```

Constants (psputility_netmodules.h:20–26):

| Constant | Value |
|---|---|
| `PSP_NET_MODULE_COMMON` | 1 (psputility_netmodules.h:20) |
| `PSP_NET_MODULE_ADHOC` | 2 (psputility_netmodules.h:21) |
| `PSP_NET_MODULE_INET` | 3 (psputility_netmodules.h:22) — not needed for ad-hoc |
| `SCE_ERROR_NET_MODULE_NOT_LOADED` | 0x80110803 (psputility_netmodules.h:29) |

We load `COMMON` then `ADHOC`. (`COMMON`+`INET` is the infrastructure-WiFi pair per the
header comment at psputility_netmodules.h:33–34; we don't need INET.)

### 1.2 Stack init

```c
int sceNetInit(int poolsize, int calloutprio, int calloutstack,
               int netintrprio, int netintrstack);          // pspnet.h:41
```

Arg meanings (pspnet.h:33–37, verbatim from doc comments):
- `poolsize` — memory pool size "for the whole of the networking library".
- `calloutprio` / `calloutstack` — priority and stack size of the **SceNetCallout** thread.
- `netintrprio` / `netintrstack` — priority and stack size of the **SceNetNetintr** thread.
- Gotcha (pspnet.h:35,37): both stack sizes **default to 4096 on non-1.5 firmware
  regardless of the value passed** — so our `4*1024` argument is effectively decorative
  on target firmware, which is fine.

```c
int sceNetAdhocInit(void);                                   // pspnet_adhoc.h:26
```

```c
int sceNetAdhocctlInit(int stacksize, int priority,
                       struct productStruct *product);       // pspnet_adhocctl.h:92
```

- Header-recommended values: `stacksize = 0x2000`, `priority = 0x30`
  (pspnet_adhocctl.h:86–87). These size/prioritize **the adhocctl thread** the library
  creates — evidence that adhocctl runs its own internal thread.
- The product struct's real name is exactly the crib sheet's guess, `struct productStruct`,
  but with one extra field (pspnet_adhocctl.h:22–30):

```c
struct productStruct {
    int  unknown;      // "set to 0, other values used are 1 and 2" (pspnet_adhocctl.h:24)
    char product[9];   // product ID string, e.g. "ULUS99999" — exactly 9 chars, no NUL room
    char unk[3];       // "possibly padding" (pspnet_adhocctl.h:28-29) — crib sheet omitted this
};                     // sizeof == 16
```

### 1.3 Event handler registration

```c
typedef void (*sceNetAdhocctlHandler)(int flag, int error, void *unknown);
                                                             // pspnet_adhocctl.h:233
int sceNetAdhocctlAddHandler(sceNetAdhocctlHandler handler, void *unknown);
                                                             // pspnet_adhocctl.h:243
int sceNetAdhocctlDelHandler(int id);                        // pspnet_adhocctl.h:252
```

- `AddHandler` returns a **handler id** (>= 0) on success (pspnet_adhocctl.h:241); keep it
  for `DelHandler` at teardown.
- The second arg of `AddHandler` is documented only as "Pass NULL" (pspnet_adhocctl.h:239).
  Note the handler typedef's third param is also `void *unknown` — the header does not
  say the AddHandler arg is passed through to the handler (likely, but unverified).
- **IMPORTANT: the SDK header defines NO event-code constants for `flag`.** A grep of the
  entire include tree finds `*_EVENT_*` constants only for apctl
  (`PSP_NET_APCTL_EVENT_*`, pspnet_apctl.h:30–32) and adhoc **matching**
  (`PSP_ADHOC_MATCHING_EVENT_*`, pspnet_adhocmatching.h:27–53), neither of which applies
  to the adhocctl handler. The crib sheet's "events: connect/disconnect/scan-complete..."
  is plausible but **not backed by any header** — event codes, the `error` param semantics,
  and the handler's execution context all go to §10 for PPSSPP cross-check. We must define
  our own named constants once verified.

State polling alternative (usable without knowing event codes):

```c
int sceNetAdhocctlGetState(int *event);                      // pspnet_adhocctl.h:124
```
"Pointer to an integer to receive the status. **Can continue when it becomes 1**"
(pspnet_adhocctl.h:120). Other state values are not enumerated in the header (§10).

### 1.4 Group connect

```c
int sceNetAdhocctlConnect(const char *name);                 // pspnet_adhocctl.h:108
```
- `name`: "maximum 8 alphanumeric characters" (pspnet_adhocctl.h:104). `"GPSP07"` (6 chars)
  is fine. Note `SceNetAdhocctlScanInfo.name` is `char[8]` with **no NUL slot** for
  8-char names (pspnet_adhocctl.h:53).
- The header also exposes an explicit host/client split we may prefer for the Join screen:

```c
int sceNetAdhocctlCreate(const char *name);                  // pspnet_adhocctl.h:133 ("as a host")
int sceNetAdhocctlJoin(struct SceNetAdhocctlScanInfo *scaninfo); // pspnet_adhocctl.h:142 ("as a client", takes a scan result)
int sceNetAdhocctlDisconnect(void);                          // pspnet_adhocctl.h:115
```
Whether `Connect` create-or-joins by name while `Create`/`Join` are the explicit forms is
not stated in the header — §10. (GameMode variants `CreateEnterGameMode` /
`JoinEnterGameMode` / `ExitGameMode` exist at pspnet_adhocctl.h:165,177,193 — not used.)

### 1.5 Verified sequence

Init (each step checked `>= 0` before proceeding):

1. `sceWlanGetSwitchState()` — refuse early with friendly UI if 0 (pspwlan.h:31).
2. `sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON)`
3. `sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC)`
4. `sceNetInit(poolsize, calloutprio, 4096, netintrprio, 4096)`
5. `sceNetAdhocInit()`
6. `sceNetAdhocctlInit(0x2000, 0x30, &product)`
7. `sceNetAdhocctlAddHandler(handler, NULL)` → save handler id
8. `sceNetAdhocctlConnect("GPSP07")` (or `Create`/`Join`), then wait until connected —
   via handler event or `sceNetAdhocctlGetState(&st)` polling for `st == 1`
9. `sceWlanGetEtherAddr(mac8)`; `sceNetAdhocPdpCreate(mac, port, bufsize, 0)`

Teardown (exact reverse; also run on HOME-exit and in the suspend path):

1. `sceNetAdhocPdpDelete(id, 0)` (pspnet_adhoc.h:55)
2. `sceNetAdhocctlDisconnect()` (pspnet_adhocctl.h:115)
3. `sceNetAdhocctlDelHandler(id)` (pspnet_adhocctl.h:252)
4. `sceNetAdhocctlTerm()` (pspnet_adhocctl.h:99)
5. `sceNetAdhocTerm()` (pspnet_adhoc.h:33)
6. `sceNetTerm()` (pspnet.h:48)
7. `sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC)` then `(PSP_NET_MODULE_COMMON)`
   (psputility_netmodules.h:49)

The headers do not themselves mandate this ordering (it mirrors init nesting);
required-ness of each step (e.g., is Disconnect-before-Term mandatory?) → §10.

---

## 2. PDP API (our data plane)

```c
int sceNetAdhocPdpCreate(unsigned char *mac, unsigned short port,
                         unsigned int bufsize, int unk1);    // pspnet_adhoc.h:45
```
- `mac` — **your own** MAC "(from sceWlanGetEtherAddr)" (pspnet_adhoc.h:38).
- `port` — "lumines uses 0x309" (pspnet_adhoc.h:39); our 0x4A4B fits `unsigned short`.
- `bufsize` — "Socket buffer size, lumines sets to 0x400" (pspnet_adhoc.h:40). This is the
  socket **receive buffer**, not a max-datagram size, as far as the header says; exact
  semantics → §10. Given our ≤ ~200 B frames, 0x400–0x2000 is the sane range to test.
- `unk1` — "Unknown, lumines sets to 0" (pspnet_adhoc.h:41). Pass 0.
- Returns the PDP object **ID** (< 0 on error) (pspnet_adhoc.h:43).

```c
int sceNetAdhocPdpSend(int id, unsigned char *destMacAddr, unsigned short port,
                       void *data, unsigned int len,
                       unsigned int timeout, int nonblock);  // pspnet_adhoc.h:70
```
- `destMacAddr` — "can be set to all 0xFF for broadcast" (pspnet_adhoc.h:61). That is our
  `RETRO_NETPACKET_BROADCAST` mapping.
- `timeout` — **microseconds** (pspnet_adhoc.h:65).
- `nonblock` — "0 to block, 1 for non-blocking" (pspnet_adhoc.h:66). Exact values, not a
  flags word.
- Returns **bytes sent**, < 0 on error (pspnet_adhoc.h:68).

```c
int sceNetAdhocPdpRecv(int id, unsigned char *srcMacAddr, unsigned short *port,
                       void *data, void *dataLength,
                       unsigned int timeout, int nonblock);  // pspnet_adhoc.h:85
```
- Note the type: `dataLength` is **`void *`** — a pointer to a length variable. The header
  calls it "the length of the data buffer" (pspnet_adhoc.h:79), implying in/out (pass
  buffer capacity in, get received size out), but in/out behavior and the pointee's width
  (int vs short) are **not stated** → §10. Return value is "Number of bytes received"
  (pspnet_adhoc.h:83), which gives us the size regardless.
- `timeout` microseconds; `nonblock` 0/1 as above (pspnet_adhoc.h:80–81).
- Nonblocking "would block" error code is not defined in these headers → §10.

```c
int sceNetAdhocPdpDelete(int id, int unk1);                  // pspnet_adhoc.h:55
```
- `unk1` — "Unknown, set to 0" (pspnet_adhoc.h:51).

```c
typedef struct pdpStatStruct {
    struct pdpStatStruct *next;   // pspnet_adhoc.h:93
    int pdpId;                    // pspnet_adhoc.h:95
    unsigned char mac[6];         // pspnet_adhoc.h:97
    unsigned short port;          // pspnet_adhoc.h:99
    unsigned int rcvdData;        // pspnet_adhoc.h:101  ("Bytes received")
} pdpStatStruct;
int sceNetAdhocGetPdpStat(int *size, pdpStatStruct *stat);   // pspnet_adhoc.h:112
```
- `size`: "Pointer to the size of the stat array (e.g 20 for one structure)"
  (pspnet_adhoc.h:107) — i.e. size **in bytes**; note sizeof(pdpStatStruct) is 20 on PSP
  (4+4+6+2+4). `rcvdData` lets us poll "is data pending" without a recv — useful for a
  poll-style RX loop if the blocking-recv thread proves awkward.
- Max PDP datagram payload size is **nowhere stated in the headers** → §10.

---

## 3. PTP API (exists; deliberately unused in v1)

`pspnet_adhoc.h` provides TCP-like streams: `sceNetAdhocPtpOpen` (:182),
`PtpConnect` (:193), `PtpListen` (:208), `PtpAccept` (:221), `PtpSend` (:234),
`PtpRecv` (:247), `PtpFlush` (:258), `PtpClose` (:268), plus `ptpStatStruct` (:273–293)
and `sceNetAdhocGetPtpStat` (:303). Same microsecond-timeout + 0/1 nonblock conventions
as PDP. Per plan §Appendix B we stay on a single PDP+ARQ path in v1; nothing in the
headers contradicts that choice.

---

## 4. Adhocctl scan & peers (Join screen)

```c
int sceNetAdhocctlScan(void);                                // pspnet_adhocctl.h:221
int sceNetAdhocctlGetScanInfo(int *length, void *buf);       // pspnet_adhocctl.h:231
```
- `Scan()` takes no args and returns immediately-styled `int`; whether completion is
  signaled via the handler (a "scan complete" event) or must be polled is **not stated**
  → §10.
- `GetScanInfo`: `length` = "length of the list", `buf` = "allocated area of size length"
  (pspnet_adhocctl.h:226–227). Whether `length` is in/out and whether a NULL-buf size
  query is supported is not stated → §10.

```c
struct SceNetAdhocctlScanInfo {
    struct SceNetAdhocctlScanInfo *next;  // pspnet_adhocctl.h:49
    int channel;                          // pspnet_adhocctl.h:51
    char name[8];                         // pspnet_adhocctl.h:53  (group name, no NUL guarantee)
    unsigned char bssid[6];               // pspnet_adhocctl.h:55
    unsigned char unknown[2];             // pspnet_adhocctl.h:57
    int unknown2;                         // pspnet_adhocctl.h:59
};
```
A scan result feeds straight into `sceNetAdhocctlJoin(scaninfo)` (pspnet_adhocctl.h:142).

```c
int sceNetAdhocctlGetPeerList(int *length, void *buf);       // pspnet_adhocctl.h:203
int sceNetAdhocctlGetPeerInfo(unsigned char *mac, int size,
                              struct SceNetAdhocctlPeerInfo *peerinfo);
                                                             // pspnet_adhocctl.h:214
struct SceNetAdhocctlPeerInfo {
    struct SceNetAdhocctlPeerInfo *next;  // pspnet_adhocctl.h:35
    char nickname[128];                   // pspnet_adhocctl.h:37
    unsigned char mac[6];                 // pspnet_adhocctl.h:39
    unsigned char unknown[6];             // pspnet_adhocctl.h:41
    unsigned long timestamp;              // pspnet_adhocctl.h:43  (units unstated → §10)
};
```
Each peer entry is ~148 bytes — budget the roster buffer accordingly (16 peers ≈ 2.4 KB).
Related helpers: `sceNetAdhocctlGetNameByAddr` (:262), `GetAddrByName` (:273),
`GetParameter(&SceNetAdhocctlParams)` (:282, struct at :71–81 — gives our current
channel/name/nickname/bssid once connected), `GetAdhocId(&productStruct)` (:151).

---

## 5. Identity / MAC handling

```c
int sceWlanGetSwitchState(void);         // pspwlan.h:31  — 0 off, 1 on (hardware switch)
int sceWlanDevIsPowerOn(void);           // pspwlan.h:24  — 0 off, 1 on (device power; distinct)
int sceWlanGetEtherAddr(uint8_t *etherAddr); // pspwlan.h:40
```
- **Gotcha (pspwlan.h:36–37):** `sceWlanGetEtherAddr` "only writes to 6 bytes, but
  requests 8 so pass it 8 bytes just in case". Use `uint8_t mac[8]` at the call site.
- MAC type across all these APIs is a bare **`unsigned char[6]` / `unsigned char *`**
  (e.g. pspnet_adhoc.h:45,70,85,97; pspnet_adhocctl.h:39,55,80). There is **no
  `SceNetEtherAddr` struct** in these headers — we typedef our own
  `typedef struct { uint8_t b[6]; } netdrv_mac_t;` internally.
- Helpers: `sceNetEtherStrton(char*, unsigned char*)` (pspnet.h:74),
  `sceNetEtherNtostr(unsigned char*, char*)` (pspnet.h:82) for logs/UI;
  `sceNetGetLocalEtherAddr(unsigned char*)` (pspnet.h:91) is a post-`sceNetInit`
  alternative to the wlan call.

---

## 6. Power & clock (plan §9)

```c
int scePowerSetClockFrequency(int pllfreq, int cpufreq, int busfreq); // psppower.h:228
```
- Constraints (psppower.h:218–226): pll 19–333, cpu 1–333, bus 1–167, and
  `cpufreq <= pllfreq`, `busfreq*2 <= pllfreq`. The plan's `(333, 333, 166)` satisfies
  all of these (166*2 = 332 ≤ 333). Valid.
- Per-knob variants exist: `scePowerSetCpuClockFrequency` (:171),
  `scePowerSetBusClockFrequency` (:177) — prefer the combined call.

```c
int scePowerGetCpuClockFrequency(void);   // psppower.h:183 (alias of ...Int, :189)
int scePowerGetBusClockFrequency(void);   // psppower.h:201
```
Log both at boot and session start per plan §9.

```c
int scePowerTick(int type);               // psppower.h:259
```
- `PSP_POWER_TICK_ALL` 0, `PSP_POWER_TICK_SUSPEND` 1, `PSP_POWER_TICK_DISPLAY` 6
  (psppower.h:51–55). During sessions call `scePowerTick(PSP_POWER_TICK_ALL)` (or
  `_SUSPEND` if we want screen dimming to still work) periodically; header states it
  prevents power-off and display-off (psppower.h:252–253). Tick period is not specified
  by the header — any sub-idle-timeout interval (e.g. once/second) works.

### Power callback (suspend/resume)

```c
typedef void (*powerCallback_t)(int unknown, int powerInfo);  // psppower.h:63
int scePowerRegisterCallback(int slot, SceUID cbid);          // psppower.h:73
int scePowerUnregisterCallback(int slot);                     // psppower.h:82
```
- `slot`: 0–15, or **-1 for auto-assign**; return is 0, or the slot number when -1 was
  passed, < 0 error (psppower.h:68–71). Keep the returned slot for Unregister.
- `cbid` comes from `sceKernelCreateCallback(name, func, arg)` (pspthreadman.h:1063),
  whose function type is `int (*SceKernelCallbackFunction)(int arg1, int arg2, void *arg)`
  (pspthreadman.h:1028). Note the **mismatch**: psppower.h's `powerCallback_t` shows two
  args and `void` return, but the callback actually registered goes through the kernel
  callback machinery with three args and `int` return — write the handler as
  `SceKernelCallbackFunction` and read `PSP_POWER_CB_*` flags from `arg2`/`powerInfo`.
  (Which arg carries `powerInfo` is implied by position but not explicitly unified across
  the two headers → §10.)
- `powerInfo` flag bits (psppower.h:27–45): `PSP_POWER_CB_POWER_SWITCH` 0x80000000,
  `PSP_POWER_CB_HOLD_SWITCH` 0x40000000, `PSP_POWER_CB_STANDBY` 0x00080000,
  `PSP_POWER_CB_RESUME_COMPLETE` 0x00040000, `PSP_POWER_CB_RESUMING` 0x00020000,
  `PSP_POWER_CB_SUSPENDING` 0x00010000, `PSP_POWER_CB_AC_POWER` 0x00001000,
  `PSP_POWER_CB_BATTERY_LOW` 0x00000100, `PSP_POWER_CB_BATTERY_EXIST` 0x00000080,
  `PSP_POWER_CB_BATTPOWER` 0x0000007F (battery-level mask).
  On `SUSPENDING`: tear the session down (radio dies in suspend anyway, plan §9).
- Kernel callbacks are only delivered while some thread is in a callback-capable wait
  (`sceKernelSleepThreadCB` pspthreadman.h:252, `...CB` wait variants, or explicit
  `sceKernelCheckCallback()` pspthreadman.h:1118). Our main loop should call
  `sceKernelCheckCallback()` once per frame or dedicate a sleeping CB thread.
- Also available and useful around SRAM flushes: `scePowerLock(0)` / `scePowerUnlock(0)`
  (psppower.h:240,249) — blocks the power switch; a toggle during lock "fires immediately
  after being unlocked" (psppower.h:233–234).

---

## 7. Threading primitives (RX thread + ring)

```c
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);  // psptypes.h:431
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry,
                             int initPriority, int stackSize, SceUInt attr,
                             SceKernelThreadOptParam *option);  // pspthreadman.h:166-167
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp); // pspthreadman.h:185
int sceKernelExitDeleteThread(int status);                      // pspthreadman.h:199
int sceKernelTerminateDeleteThread(SceUID thid);                // pspthreadman.h:217
int sceKernelWaitThreadEnd(SceUID thid, SceUInt *timeout);      // pspthreadman.h:298
int sceKernelDelayThread(SceUInt delay);                        // pspthreadman.h:320 (microseconds; 1000000 = 1 s, :317)
```
- **Priority: smaller number = higher priority** ("Less if higher priority",
  pspthreadman.h:161); header example uses 0x18 (pspthreadman.h:154). Main threads are
  commonly 0x20-ish; the RX thread should be numerically just below main's value.
- `attr`: pass `PSP_THREAD_ATTR_USER` 0x80000000 (pspthreadman.h:51); `PSP_THREAD_ATTR_VFPU`
  0x4000 (:48) is irrelevant for the RX thread. `option` NULL.

```c
SceInt64 sceKernelGetSystemTimeWide(void);                      // pspthreadman.h:1588
```
- The header does not state the unit (§10) — but `sceKernelDelayThread` docs
  (pspthreadman.h:317) and every timeout in the net API are microseconds, and
  `sceKernelSysClock2USecWide` (pspthreadman.h:1572) converts sysclocks to µs; treat as
  µs pending cross-check. Fine for ARQ timers either way (we only need monotonic deltas).

### Event flags (RX ring "data available" signal)

```c
SceUID sceKernelCreateEventFlag(const char *name, int attr, int bits,
                                SceKernelEventFlagOptParam *opt); // pspthreadman.h:723
int sceKernelSetEventFlag(SceUID evid, u32 bits);                 // pspthreadman.h:733
int sceKernelClearEventFlag(SceUID evid, u32 bits);               // pspthreadman.h:743
int sceKernelPollEventFlag(int evid, u32 bits, u32 wait, u32 *outBits); // pspthreadman.h:755
int sceKernelWaitEventFlag(int evid, u32 bits, u32 wait, u32 *outBits,
                           SceUInt *timeout);                     // pspthreadman.h:766 (timeout µs, :764)
int sceKernelDeleteEventFlag(int evid);                           // pspthreadman.h:787
```
- Wait types (pspthreadman.h:699–706): `PSP_EVENT_WAITAND` 0, `PSP_EVENT_WAITOR` 1,
  `PSP_EVENT_WAITCLEAR` 0x20.
- **Gotcha:** default attr allows only a **single** waiting thread
  (`PSP_EVENT_WAITSINGLE` 0x00); pass `PSP_EVENT_WAITMULTIPLE` 0x200 if more than one
  thread may ever wait (pspthreadman.h:692–694).

### Semaphores (alternative)

```c
SceUID sceKernelCreateSema(const char *name, SceUInt attr, int initVal,
                           int maxVal, SceKernelSemaOptParam *option); // pspthreadman.h:512
int sceKernelSignalSema(SceUID semaid, int signal);                    // pspthreadman.h:536
int sceKernelWaitSema(SceUID semaid, int signal, SceUInt *timeout);    // pspthreadman.h:552
int sceKernelDeleteSema(SceUID semaid);                                // pspthreadman.h:520
```

For our design (RX thread blocks in `PdpRecv`, main thread drains a SPSC ring) no kernel
signaling is strictly required — the ring is polled at frame boundaries. An event flag
becomes useful only if we later want the main thread to *sleep* on RX.

---

## 8. Corrected Appendix B crib sheet

Same shape as the plan's block; every line verified against headers. `// OK` = original
was right; `// FIX` = corrected; `// NOTE` = original right but under-specified.

```c
// Module load (user mode, CFW):                                   // OK
sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);   // OK — =1, psputility_netmodules.h:20, fn :40
sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC);    // OK — =2, psputility_netmodules.h:21

// Stack init:
sceNetInit(128*1024, 42, 4*1024, 42, 4*1024);
// OK — (poolsize, calloutprio, calloutstack, netintrprio, netintrstack), pspnet.h:41.
// NOTE: stack args are forced to 4096 on non-1.5 firmware anyway (pspnet.h:35,37).

sceNetAdhocInit();                                // OK — (void), pspnet_adhoc.h:26

struct productStruct product = { 0, "GPSPADHOC" };
sceNetAdhocctlInit(0x2000, 0x30, &product);
// FIX (crib guessed right on shape): int sceNetAdhocctlInit(int stacksize, int priority,
//   struct productStruct*), pspnet_adhocctl.h:92. Header recommends 0x2000/0x30 (:86-87).
// FIX: struct is { int unknown; char product[9]; char unk[3]; } — crib omitted unk[3]
//   (pspnet_adhocctl.h:22-30). product[9] holds exactly 9 chars (no NUL), e.g. "ULUS99999".

int hid = sceNetAdhocctlAddHandler(handler, NULL);
// FIX: returns handler id (keep for DelHandler); 2nd arg documented "Pass NULL"
//   (pspnet_adhocctl.h:239-243). Handler typedef:
//   void handler(int flag, int error, void *unknown);   // pspnet_adhocctl.h:233
// FIX: crib's "events: connect/disconnect/scan-complete..." — NO event constants exist in
//   any SDK header for adhocctl. flag values = open question (PPSSPP cross-check).

sceNetAdhocctlConnect("GPSP07");
// OK — (const char*), max 8 alphanumeric chars, pspnet_adhocctl.h:104-108.
// NOTE: explicit host/client forms also exist: sceNetAdhocctlCreate(name) (:133),
//   sceNetAdhocctlJoin(&scaninfo) (:142). Wait for connected state via handler or
//   sceNetAdhocctlGetState(&st) until st==1 (:120-124).

// Identity & peers:
sceWlanGetSwitchState();                          // OK — (void), 0 off / 1 on, pspwlan.h:31
uint8_t mac[8]; sceWlanGetEtherAddr(mac);
// FIX: pass an 8-byte buffer — writes 6 bytes but "requests 8" (pspwlan.h:36-40).

sceNetAdhocctlGetPeerList(&len, buf);
// OK — (int *length, void *buf), pspnet_adhocctl.h:203. Entries are
//   SceNetAdhocctlPeerInfo { next, nickname[128], mac[6], unknown[6], timestamp }
//   (:33-44) — ~148 B each. length in/out semantics unstated (open question).

// PDP datagrams (our data plane):
int pdp = sceNetAdhocPdpCreate(mac, 0x4A4B, 0x2000, 0);
// OK — (unsigned char *mac /*own MAC*/, unsigned short port, unsigned int bufsize,
//   int unk1 /*0*/), pspnet_adhoc.h:45. bufsize = socket buffer (lumines: 0x400, :40).

sceNetAdhocPdpSend(pdp, dst_mac, 0x4A4B, data, len, timeout_us, nonblock);
// OK — pspnet_adhoc.h:70. dst all-0xFF = broadcast (:61); timeout in MICROSECONDS (:65);
//   nonblock: 0 = block, 1 = nonblock (:66); returns bytes sent (:68).

int rlen = bufcap;
sceNetAdhocPdpRecv(pdp, src_mac, &port, buf, &rlen, timeout_us, nonblock);
// OK-ish — pspnet_adhoc.h:85. FIX: dataLength param is typed `void *` (pointer to a
//   length variable); in/out width unstated (open question). Returns bytes received (:83).

sceNetAdhocPdpDelete(pdp, 0);                     // ADDED — (int id, int unk1=0), pspnet_adhoc.h:55

// Teardown mirrors init in reverse; always Disconnect before Term; handle HOME-exit too.
// OK as policy — full order: PdpDelete → sceNetAdhocctlDisconnect() (:115) →
//   sceNetAdhocctlDelHandler(hid) (:252) → sceNetAdhocctlTerm() (:99) →
//   sceNetAdhocTerm() (pspnet_adhoc.h:33) → sceNetTerm() (pspnet.h:48) →
//   sceUtilityUnloadNetModule(ADHOC, then COMMON) (psputility_netmodules.h:49).
```

---

## 9. Divergences and risks

1. **No adhocctl event constants in the SDK.** The plan assumes named
   connect/disconnect/scan events; the header gives only `(int flag, int error, void*)`
   (pspnet_adhocctl.h:233). We must hardcode values verified from PPSSPP and name them
   ourselves. Until then, `sceNetAdhocctlGetState` polling (st==1 = proceed,
   pspnet_adhocctl.h:120) is the only header-blessed readiness check.
2. **Handler execution context unknown.** `sceNetAdhocctlInit` takes stack/priority for
   an internal adhocctl thread (pspnet_adhocctl.h:86–87), strongly implying the handler
   runs on that thread, not ours — so the handler must only set flags/post to the ring,
   never call core callbacks. Header doesn't state it outright → cross-check.
3. **`productStruct` has a 4th field** (`char unk[3]`, pspnet_adhocctl.h:29) the plan's
   crib omitted; sizeof is 16. Zero-init the whole struct. `unknown` takes 0/1/2 with
   unexplained meaning (:24).
4. **`sceWlanGetEtherAddr` wants an 8-byte buffer** (pspwlan.h:36–37). A `mac[6]` local
   is a latent 2-byte stack smash.
5. **PdpRecv's length param is `void *`** (pspnet_adhoc.h:85) — the compiler will accept
   anything; a wrong-width pointee is silent corruption. Wrap it once in transport_adhoc.c
   and test the width against PPSSPP behavior.
6. **`sceNetInit` stack-size args are ignored on non-1.5 firmware** (pspnet.h:35,37) —
   harmless, but don't "tune" them expecting effect.
7. **No max PDP payload constant anywhere** — our ≤ ~200 B frames are almost certainly
   fine, but the actual MTU/limit needs cross-check before we assert on oversize.
8. **Power callback type mismatch** between `powerCallback_t` (2 args, void — psppower.h:63)
   and `SceKernelCallbackFunction` (3 args, int — pspthreadman.h:1028); and callbacks only
   fire from CB-wait states or `sceKernelCheckCallback()` (pspthreadman.h:1118) — the main
   loop must pump callbacks or suspend handling silently never runs.
9. **Plan-internal inconsistency:** §9 says default 333 MHz during sessions; risk-register
   row 6 says "222 during sessions by default". §9 is the governing text; flagging so
   nobody implements row 6. Header confirms `(333,333,166)` is within documented limits
   (psppower.h:218–226).
10. **`SceNetAdhocctlScanInfo.name` / group names are `char[8]` with no NUL slot**
    (pspnet_adhocctl.h:53) — UI code must copy with explicit bounds, never `strcpy`/`%s`.
11. **Event flags default to single-waiter** (`PSP_EVENT_WAITSINGLE`,
    pspthreadman.h:692) — a second waiter errors unless created with
    `PSP_EVENT_WAITMULTIPLE` (0x200).
12. **Adhoc matching library exists** (`pspnet_adhocmatching.h` — `sceNetAdhocMatchingInit`
    :97, `Create` :124, callback typedef :107, full `PSP_ADHOC_MATCHING_EVENT_*` set
    :27–53). It duplicates much of our JOIN/WELCOME session layer. We deliberately keep our
    own PDP-based protocol (portable to the UDP desktop backend), but if session
    establishment fights us on hardware, matching is the fallback — recording it here so
    the option isn't forgotten.

## 10. Needs cross-check against PPSSPP source / known-good homebrew

Behavioral facts the headers do not state — **do not code against guesses**.
Status after PPSSPP cross-check (see §11): **ANSWERED→§11.x** = verified against PPSSPP
master @ d90fdee8; **STILL-OPEN (hw)** = only real hardware can settle it.

1. Adhocctl handler `flag` event codes — **ANSWERED→§11.1**.
2. Adhocctl handler `error` param semantics; AddHandler arg passthrough — **ANSWERED→§11.1/§11.2**.
3. Which thread/context the adhocctl handler runs in — **ANSWERED→§11.2**.
4. `sceNetAdhocctlGetState` full state enumeration — **ANSWERED→§11.3**.
5. `sceNetAdhocctlConnect` vs `Create`/`Join` — **ANSWERED→§11.4** (all three are the same op in PPSSPP).
6. `GetScanInfo` / `GetPeerList` `length` semantics — **ANSWERED→§11.5**.
7. `sceNetAdhocctlScan` completion delivery / rescan rules — **ANSWERED→§11.6**.
8. `sceNetAdhocPdpRecv` `dataLength` pointee width and in/out behavior — **ANSWERED→§11.7** (s32, in/out).
9. Would-block / timeout error codes — **ANSWERED→§11.7/§11.8** (0x80410709 / 0x80410715).
10. Max PDP datagram payload / oversize behavior — **ANSWERED→§11.8** (MFS 1444; PPSSPP itself doesn't enforce).
11. `pdpStatStruct.rcvdData` meaning — **ANSWERED→§11.10** (pending bytes, capped at bufsize).
12. Teardown-order violations (Term without Disconnect) — **ANSWERED for PPSSPP→§11.12**
    (Term auto-disconnects); **STILL-OPEN (hw)** whether real fw is as forgiving.
13. `sceKernelGetSystemTimeWide` unit — **ANSWERED→§11.13** (microseconds).
14. `SceNetAdhocctlPeerInfo.timestamp` units/epoch — **ANSWERED→§11.5** (u64 µs, same clock
    as SystemTimeWide; NOTE the pspsdk struct is 4 bytes too short vs PPSSPP's 152-byte entry).
15. `productStruct.unknown` meaning of 0/1/2 — **PARTIAL→§11.14** (it's an adhoc-ID `type`;
    2 = GameSharing per PPSSPP comment); exact enum **STILL-OPEN (hw)**.
16. Power callback arg carrying `PSP_POWER_CB_*` bits — **ANSWERED→§11.15** (arg2/notifyArg);
    suspend delivery timing — **STILL-OPEN (hw)** (PPSSPP doesn't emulate suspend).
17. Practical minimum `sceNetInit` poolsize — **PARTIAL→§11.16** (PPSSPP: any nonzero works;
    priorities must be 0x08–0x77); real-hw minimum **STILL-OPEN (hw)**.

---

## 11. PPSSPP cross-check results (master @ d90fdee8)

Source: PPSSPP master @ d90fdee8 (2026-07-30). All paths relative to the ppsspp repo root.
PPSSPP is our first runtime target; where its comments document real-hardware divergence,
that is quoted explicitly.

### 11.1 Adhocctl handler event codes and error codes

Event codes (`Core/HLE/proAdhoc.h:64-71`):

```c
#define ADHOCCTL_EVENT_ERROR         0  // "Used to pass error code to Adhocctl Handler?"
#define ADHOCCTL_EVENT_CONNECT       1
#define ADHOCCTL_EVENT_DISCONNECT    2
#define ADHOCCTL_EVENT_SCAN          3  // scan COMPLETED (not started)
#define ADHOCCTL_EVENT_GAME          4  // GameMode entered
#define ADHOCCTL_EVENT_DISCOVER      5
#define ADHOCCTL_EVENT_WOL           6
#define ADHOCCTL_EVENT_WOL_INTERRUPT 7
```

Handler `error` param: 0 for normal events; for `ADHOCCTL_EVENT_ERROR` it carries an SCE
error code — observed emissions: `SCE_NET_ADHOCCTL_ERROR_ALREADY_CONNECTED` (0x80410b02)
when Create/Connect/Scan is called while already connected (`Core/HLE/sceNetAdhoc.cpp:3160,3649`,
`:3646` comment: "When tested with JPCSP + official prx files it seems when adhocctl in a
connected state ... attempting to create/connect/join/scan will return a success (without
doing anything?)" — i.e. HLE returns 0 and the error arrives via the handler), and
`SCE_NET_ADHOC_ERROR_TIMEOUT` (0x80410715) on server-login timeout (`Core/HLE/proAdhoc.cpp:1484`).

Full error-code tables (`Core/HLE/ErrorCodes.h`):
- adhoc (0x804107xx): `:576-603` — INVALID_SOCKET_ID 0x80410701, INVALID_ADDR 0x80410702,
  INVALID_PORT 0x80410703, INVALID_BUFLEN 0x80410704, INVALID_DATALEN 0x80410705,
  **NOT_ENOUGH_SPACE 0x80400706 ("not a typo" — 0x8040, not 0x8041)**, SOCKET_DELETED
  0x80410707, SOCKET_ALERTED 0x80410708, **WOULD_BLOCK 0x80410709**, PORT_IN_USE 0x8041070a,
  DISCONNECTED 0x8041070c, INVALID_ARG 0x80410711, NOT_INITIALIZED 0x80410712,
  ALREADY_INITIALIZED 0x80410713, BUSY 0x80410714, **TIMEOUT 0x80410715**.
- adhocctl (0x80410bxx): `:632-650` — ALREADY_CONNECTED 0x80410b02, WLAN_SWITCH_OFF
  0x80410b03, INVALID_ARG 0x80410B04, TIMEOUT 0x80410b05, ALREADY_INITIALIZED 0x80410b07,
  NOT_INITIALIZED 0x80410b08, DISCONNECTED 0x80410b09, BUSY 0x80410b10,
  TOO_MANY_HANDLERS 0x80410b12, STACKSIZE_TOO_SHORT 0x80410B13.

Max handlers: `MAX_ADHOCCTL_HANDLERS = 32` (`Core/HLE/proAdhoc.h:526`); exceeding returns
TOO_MANY_HANDLERS (`Core/HLE/sceNetAdhoc.cpp:3324`). Handler ids count up from 0.

### 11.2 Handler execution context — dedicated PSP thread, NOT kernel callbacks

The handler is **not** a `sceKernelCreateCallback`-style callback and needs **no
`sceKernelCheckCallback` pumping**. Mechanism:

- `sceNetAdhocctlInit` creates a real (emulated) **PSP user thread named "AdhocThread"**
  using the app-supplied `stackSize`/`prio` args (`Core/HLE/sceNetAdhoc.cpp:1986-1992`:
  `__KernelCreateThread("AdhocThread", ..., prio, stackSize, PSP_THREAD_ATTR_USER, ...)`).
- That thread loops on a hidden syscall `__NetTriggerCallbacks` (`Core/HLE/sceNet.cpp:595`,
  loop stub via `__CreateHLELoop`), which drains a queued event list `adhocctlEvents`
  (producers call `notifyAdhocctlHandlers()` → `__UpdateAdhocctlHandlers()`, which only
  enqueues under a mutex — `Core/HLE/sceNetAdhoc.cpp:1887-1890`).
- Per event, every registered handler is invoked as a MIPS call **on the AdhocThread**
  with `args = { flag, error, handlerArg }` (`Core/HLE/sceNetAdhoc.cpp:5804-5806,5863-5868`).
  **AddHandler's second argument IS passed through as the handler's third param**
  (`:3313-3314` stores it; `:5865` `args[2] = it->second.argument`).
- After dispatch, the new adhocctl state is applied via a scheduled event with per-event
  delays (`ScheduleAdhocctlState`, `:5872`), then the thread sleeps
  `sceKernelDelayThread(adhocDefaultDelay=10000µs)` (`:5879`; delays in
  `Core/HLE/NetAdhocCommon.h:35-43`, e.g. `adhocEventDelay = 2000000 // "2000000 on real PSP ?"`).

Consequences for us: the handler runs concurrently with our main thread at the priority we
pass to `sceNetAdhocctlInit` — it must stay flag-set-only, exactly as §9.2 assumed.
`Core/HLE/proAdhoc.cpp:1278`: "Make sure MIPS calls have been fully executed before the
next notifyAdhocctlHandlers" — events are serialized; one handler invocation completes
before the next event fires.

### 11.3 GetState state machine

`Core/HLE/proAdhoc.h:74-79`:

```c
#define ADHOCCTL_STATE_DISCONNECTED 0
#define ADHOCCTL_STATE_CONNECTED    1   // matches header "can continue when it becomes 1"
#define ADHOCCTL_STATE_SCANNING     2
#define ADHOCCTL_STATE_GAMEMODE     3
#define ADHOCCTL_STATE_DISCOVER     4
#define ADHOCCTL_STATE_WOL          5
```

`sceNetAdhocctlGetState` writes the current state as u32 and returns 0
(`Core/HLE/sceNetAdhoc.cpp:2015-2030`). Event→state mapping in `__NetTriggerCallbacks`
(`:5818-5861`): CONNECT→CONNECTED, SCAN→DISCONNECTED (i.e. after a completed scan you are
back to 0, not some "scan done" state), DISCONNECT→DISCONNECTED, GAME→GAMEMODE.

### 11.4 Connect vs Create/Join; group-name rules

All three funnel into the same `NetAdhocctl_Create()` (`Core/HLE/sceNetAdhoc.cpp:3712-3772`):
`Connect` = `Create` with a different `adhocConnectionType` tag (affects only internal event
delays), and `Join` just extracts `group_name` from the scan info and calls the same path.
So in PPSSPP **Connect is literally create-or-join by name**. Comment at `:3760`: "Adhoc
Server may need to be changed to differentiate between Host/Create and Join, otherwise it
can't support multiple Host using the same Group name".

Group-name validation `validNetworkName` (`Core/HLE/proAdhoc.cpp:2392-2410`): up to
`ADHOCCTL_GROUPNAME_LEN = 8` chars, each must be `[0-9A-Za-z]` (NUL terminates early);
violation returns `SCE_NET_ADHOC_ERROR_INVALID_ARG` 0x80410711 (note: the *adhoc*, not
*adhocctl*, code — `sceNetAdhoc.cpp:3701`). `"GPSP07"` is valid.

State rules (`:3646-3697`): calling while CONNECTED/GAMEMODE → HLE returns 0 but handler
gets `EVENT_ERROR/ALREADY_CONNECTED`; while busy (transition in flight) →
`SCE_NET_ADHOCCTL_ERROR_BUSY` 0x80410b10. Hardware quirk comment (`:3692`): "When tested
using JPCSP + official prx files it seems sceNetAdhocctlCreate switching to a different
thread for at least 100ms after returning success".

### 11.5 GetScanInfo / GetPeerList buffer semantics

Both use identical **in/out byte-count** semantics (`Core/HLE/sceNetAdhoc.cpp:3201-3299`
for GetScanInfo, `:5957-6049` for GetPeerList):

- `length` pointee is **s32, in BYTES**.
- **`buf == NULL` is a supported size query**: writes `count * sizeof(entry)` into `*length`,
  returns 0 (`:3231-3235`, `:5979-5983`).
- With a buffer: entries filled = `min(*length / sizeof(entry), available)`; buffer is
  zeroed first; `next` fields are written as **absolute PSP addresses** of the following
  entry within your buffer, last entry `next = 0` (`:3271-3277`, `:6023-6028`); on return
  `*length` = bytes actually written (`:3281`, `:6032`).
- GetScanInfo returns `*length = 0` when already CONNECTED/GAMEMODE (`:3226-3229`, comment
  "FIXME: When already connected to a group GetScanInfo will return size = 0 ?").
- GetPeerList excludes timed-out peers (`:5977,6005`).

**Struct layouts PPSSPP actually writes** (packed; `Core/HLE/proAdhoc.h:197-203,233-240`):

```c
SceNetAdhocctlScanInfoEmu {           // 28 bytes
    u32 next; s32 channel; u8 group_name[8]; u8 bssid[6]; u8 pad[2]; s32 mode; };
SceNetAdhocctlPeerInfoEmu {           // 152 bytes  <-- pspsdk's struct is 148!
    u32 next; u8 nickname[128]; u8 mac[6]; u16 padding;
    u32 flags;      // PPSSPP writes 0x0400 ("00 04 00 00 on KHBBS")
    u64 last_recv;  // µs, same clock as sceKernelGetSystemTimeWide (proAdhoc.h:239)
};
```

**Divergence flag:** the pspsdk `SceNetAdhocctlPeerInfo` (§4) ends `unknown[6]; unsigned
long timestamp;` = 148 bytes, but PPSSPP writes 152-byte entries (u16 pad + u32 flags +
**u64** timestamp). Size our roster buffer with the 152-byte stride and read `last_recv`
as u64 µs. This also answers §10.14: timestamp = microseconds on the
`sceKernelGetSystemTimeWide` clock (comment `proAdhoc.h:239`), not wall-clock epoch.

### 11.6 Scan lifecycle

`sceNetAdhocctlScan` (`Core/HLE/sceNetAdhoc.cpp:3148-3199`): only starts a scan from
DISCONNECTED (sets state = SCANNING); comment `:3165`: "Only scan when in Disconnected
state, otherwise AdhocServer will kick you out". While CONNECTED → returns 0 + handler
`EVENT_ERROR/ALREADY_CONNECTED`; mid-transition → returns BUSY 0x80410b10. Completion is
delivered **via the handler**: when the AdhocServer sends OPCODE_SCAN_COMPLETE the
friendFinder thread fires `notifyAdhocctlHandlers(ADHOCCTL_EVENT_SCAN, 0)`
(`Core/HLE/proAdhoc.cpp:1740-1754`), and the SCAN event resets state to DISCONNECTED
(`sceNetAdhoc.cpp:5826-5827`) — after which rescanning or Connect is legal.

### 11.7 PdpRecv semantics (the big one)

`Core/HLE/sceNetAdhoc.cpp:2531-2741` (blocking helper `DoBlockingPdpRecv` `:775-870`):

- **`dataLength` pointee is `int` (s32)**, in/out: pass buffer capacity in, receive actual
  datagram size out (`:2544` `int *len = (int *)dataLength;`, `:2715` `*len = received`).
- **`port` pointee is `u16`** — comment `:2543`: "Looking at Quake3 sourcecode (net_adhoc.c)
  this is an 'int' (32bit) but changing here to 32bit will cause FF-Type0 to see duplicated
  Host" — treat as u16, upper half undefined.
- **Return value on success is 0, NOT byte count** — comment `:2726-2727`: "According to
  pspsdk-1.0+beta2 returned value is Number of bytes received, but JPCSP returning 0? ...
  Returning number of bytes received will cause KH BBS unable to see the game event/room".
  Also `:2715` "Kurok homebrew seems to use the new value of len than returned value as
  data length". → **Our wrapper must read the length from `*dataLength`, never the return.**
- Nonblock (`flag=1`) with nothing pending → `SCE_NET_ADHOC_ERROR_WOULD_BLOCK` 0x80410709
  (`:2699-2700`).
- Blocking (`flag=0`): PPSSPP simulates the block (thread wait + 0.5 ms retry event,
  `:1697-1703`); on expiry of a nonzero `timeout` → `SCE_NET_ADHOC_ERROR_TIMEOUT`
  0x80410715 (`:840-843`). `timeout=0` blocks indefinitely.
- **Buffer too small**: returns `SCE_NET_ADHOC_ERROR_NOT_ENOUGH_SPACE` **0x80400706**,
  sets `*len` to the required datagram size, and (in the non-relay path) copies the first
  `min(received, capacity)` bytes into your buffer while the datagram **remains queued**
  (peek-based, `:2661-2680`) — retry with a bigger buffer to get the whole thing.
- **No partial reads on success**: "PDP always sent in full size or nothing ... If available
  UDP data is larger than buffer, excess data is lost." (`:2627`) — datagram framing is
  preserved, one PdpRecv = one datagram.
- Received datagrams from senders not in the peer table are silently discarded (`:2634-2657`).

### 11.8 PdpSend: max payload, broadcast, return codes

`Core/HLE/sceNetAdhoc.cpp:2283-2518`:

- **Return value on success is 0, not bytes sent** — `:2390`: "MotorStorm will try to
  resend if return value is not 0". (pspsdk header's "returns bytes sent" is wrong for
  PPSSPP behavior.)
- **PPSSPP enforces no maximum payload** (only `len >= 0` is checked, `:2304`; oversize
  would surface as an OS-level sendto failure). The PSP-derived constants are
  `Core/HLE/proAdhoc.h:127-134`: **`PSP_ADHOC_PDP_MFS 1444` ("PDP Maximum Fragment Size")**,
  `PSP_ADHOC_PDP_MTU 65523`, `PSP_ADHOC_PTP_MSS 1444`. Treat **1444 bytes as the hardware
  per-datagram budget**; our ≤ ~200 B frames are far inside it.
- UDP comment `:2346`: "UDP are guaranteed to be sent as a whole or nothing (failed if
  len > SO_MAX_MSG_SIZE), and never be partially sent/recv".
- **Broadcast MAC (FF:FF:FF:FF:FF:FF)**: PPSSPP expands it to one unicast UDP send per
  known group peer (peer list comes from the AdhocServer), skipping timed-out peers
  (`:2404-2434`); `:2491`: "Success, Broadcast never fails!" — returns 0 even with zero
  peers. **A broadcast right after joining, before the peer list has populated, is
  silently sent to nobody** — our JOIN/WELCOME retry loop must tolerate that.
- Zero MAC (all-00) destination → `SCE_NET_ADHOC_ERROR_INVALID_PORT`-adjacent path: fails
  `!isZeroMAC` check → INVALID_ADDR 0x80410702 (`:2315,2497`); `dport == 0` →
  INVALID_PORT 0x80410703 (`:2302,2513`). Unknown (unresolvable) unicast MAC → **fakes
  success**, returns 0 (`:2400-2401`).
- Nonblock send with full buffer → WOULD_BLOCK 0x80410709 (`:2394-2395`); blocking send
  that can't complete → TIMEOUT 0x80410715 with comment "Does PDP can Timeout? There is no
  concept of Timeout when sending UDP due to no ACK" (`:2397-2398`).

### 11.9 scePowerSetClockFrequency (PPSSPP)

`Core/HLE/scePower.cpp:434-478`: fully implemented with validation:
`pll` must be 19–333 and ≥ `cpu` (comment `:435`: "190 might (probably) be a typo for 19,
but it's what the actual PSP validates against"), `cpu` 1–333, **`bus` 1–166** (not 167;
`:442`) — violations return `SCE_KERNEL_ERROR_INVALID_VALUE`. `busfreq` is otherwise
ignored ("It seems like the busfreq parameter has no effect (but can cause errors.)",
`:446`; bus is derived as pll/2 → 166.5 MHz at 333). A PLL change delays the calling
thread ~150 ms (`:458-466`). If the user has locked CPU speed in PPSSPP settings the call
still **returns 0** but is ignored (`:475`). **Our `(333,333,166)` call passes validation
and our harness assert of clock=333 will hold** (`scePowerGetCpuClockFrequency` reflects
the set value unless user-locked).

### 11.10 PdpCreate bufsize & RX queue depth

`Core/HLE/sceNetAdhoc.cpp:2081-2242`:

- `bufsize` is kept as `socket->buffer_size` and mapped onto the host UDP socket as
  **SO_SNDBUF = bufsize×5, SO_RCVBUF = bufsize×10** (`:2121-2125`), with comments: "Send
  Buffer should be smaller than Recv Buffer to prevent faster device from flooding slower
  device too much" / "too large may cause slow performance on Warriors Orochi 2".
- **Datagrams DO queue**: the receive queue is the OS UDP buffer, so multiple pending
  datagrams are held (≈ bufsize×10 bytes worth at host level). One PdpRecv pops one.
- `GetPdpStat.rcvdData` (`rcv_sb_cc`, `:3976-3997`) = **currently pending readable bytes**
  (FIONREAD, with a peek fallback), **capped to `buffer_size`** — hardware comment `:3977`:
  "It seems real PSP respecting the socket buffer size arg, so we may need to cap the
  value up to the buffer size arg". So on real hardware assume the queue really is
  `bufsize` bytes deep — **size our PdpCreate bufsize ≥ (max frame × expected burst)**;
  0x2000 comfortably holds > 40 of our ≤200 B frames. This answers §10.11: pending bytes,
  not a lifetime counter.
- `port = 0` means "auto/client port" (`:2102-2106`). Port collisions →
  PORT_IN_USE 0x8041070a (`:2097-2100`). Max sockets: `MAX_SOCKET 255` with hw comment
  "PSP might not allows more than 255 sockets? Hotshots Tennis doesn't seems to works with
  socketId > 255" (`proAdhoc.h:766`). PdpCreate **requires `sceNetInit` done**
  (`:2087-2088`, returns 0x800201CA otherwise) and a prior successful
  Create/Connect/Join (`:2111-2112` FIXME: "MAC only valid after successful attempt to
  Create/Connect/Join a Group?") — i.e. **create the PDP socket only after adhocctl reports
  CONNECTED**, which our §1.5 sequence already does.

### 11.11 sceUtilityLoadNetModule / sceNetInit

- `sceUtilityLoadNetModule` (`Core/HLE/sceUtility.cpp:1316-1341`) is **FAKE**: it loads HLE
  stub prx entries (so `sceKernelGetModuleIdList`-style checks pass) and always returns 0.
  Unload is a pure no-op (`:1343-1345`). **PPSSPP does not error if you skip it** — but
  keep the calls: `sceNetAdhocctlInit` carries the hardware note (`sceNetAdhoc.cpp:1969`)
  "FIXME: Returning 0x8002013a (SCE_KERNEL_ERROR_LIBRARY_NOT_YET_LINKED) without adhoc
  module loaded first?" — real firmware needs the module loaded.
- `sceNetInit` (`Core/HLE/sceNet.cpp:963-1015`): poolsize 0 →
  `SCE_KERNEL_ERROR_ILLEGAL_MEMSIZE`; **callout/netintr priorities must be 0x08–0x77**
  else `SCE_KERNEL_ERROR_ILLEGAL_PRIORITY` (`:975-979`) — our crib's `42` (0x2A) is fine.
  Pool is allocated from user RAM but otherwise faked (`netMallocStat.pool = poolsize-0x20`,
  `:1003-1005`, hw observation from Vantage Master: game passed 0x20000 = 128 KiB,
  `proAdhoc.h:170`). §10.17: any nonzero pool works in PPSSPP; keep 128 KiB for hardware.

### 11.12 Teardown-order tolerance (PPSSPP)

`NetAdhocctl_Term` auto-disconnects if still connected (`Core/HLE/sceNetAdhoc.cpp:3448-3456`)
and clears handlers itself (`:3477`); `NetAdhoc_Term` calls `NetAdhocctl_Term` first and
deletes all sockets (`:3896-3906`). So in PPSSPP, Term-without-Disconnect is safe. Real
firmware behavior remains unverified — keep our strict reverse-order teardown (§1.5).
Note `:3464` TODO: "May need to block current thread to make sure all Adhocctl callbacks
have been fully executed before terminating" — don't call Term from inside the handler.

### 11.13 sceKernelGetSystemTimeWide unit

Microseconds confirmed: returns `CoreTiming::GetGlobalTimeUsScaled()`
(`Core/HLE/sceKernelTime.cpp:92-99`). Same clock used for peer `last_recv` (§11.5).

### 11.14 productStruct / adhoc ID

PPSSPP's name for it is `SceNetAdhocctlAdhocId { s32 type; u8 data[9]; u8 padding[3]; }`
(`Core/HLE/proAdhoc.h:354-360`) — matches pspsdk's 16-byte layout. The first field is a
**type**, comment: "Air Conflicts - Aces Of World War 2 is using 2 for GameSharing?" —
so 0 = normal game product code, 2 = game-sharing; use 0. PPSSPP stores it but the value
does not gate anything in the emulation path (`sceNetAdhoc.cpp:1976-1980`).

### 11.15 Power callback arg position

`scePowerRegisterCallback` immediately fires `__KernelNotifyCallback(cbId, arg)` with
`arg = PSP_POWER_CB_AC_POWER | PSP_POWER_CB_BATTERY_EXIST | PSP_POWER_CB_BATTERY_FULL`
(`Core/HLE/scePower.cpp:239-242`) — the flags travel as the kernel callback's **notifyArg,
i.e. `arg2`** of `SceKernelCallbackFunction(arg1, arg2, common)`. Expect an immediate
battery/AC status callback right after registration. Suspend/resume delivery timing is
not emulated (STILL-OPEN for hardware).

### 11.16 Running two instances against the built-in AdhocServer (pointers for the config task)

- **Instance id**: `PPSSPP_ID` (1-based) via shared-memory counter, `Core/Instance.cpp:129-164`.
- **MAC uniqueness is automatic for instance ≥ 2**: PPSSPP overrides the configured MAC
  with all-six-bytes = `PPSSPP_ID` (`Core/HLE/sceNet.cpp:1035-1036`,
  `Core/HLE/proAdhoc.cpp:1941-1942`); instance 1 uses `Config sMACAddress`
  ("MacAddress", auto-randomized if malformed, `Core/Config.cpp:1098,1599-1600`).
- **Loopback IP per instance**: `127.0.0.PPSSPP_ID` — literally
  `0x7F000001 + PPSSPP_ID - 1` (`Core/HLE/sceNet.cpp:517-527`); when the server address is
  localhost/127.* (`isLocalServer`), PDP sockets bind to that per-instance IP
  (`sceNetAdhoc.cpp:2140-2142`), so two instances never fight over a UDP 5-tuple.
- **Port shifting**: `PortOffset` config (default **10000**, `Core/Config.cpp:1069`);
  real UDP port = PSP port + offset (`sceNetAdhoc.cpp:2143`); offsets are exchanged
  per-peer via the server so instances may differ.
- **Built-in AdhocServer**: enabled by `EnableAdhocServer` (default false,
  `Core/Config.cpp:1065`), thread starts in `__NetAdhocInit`
  (`sceNetAdhoc.cpp:1911-1914`), listens on **TCP 27312** (`proAdhoc.h:44 SERVER_PORT`).
  A second instance detects the port is already served and skips cleanly:
  "AdhocServer: Skipped starting because the server is already available"
  (`Core/HLE/proAdhocServer.cpp:1668-1671`). So: set `proAdhocServer = localhost` +
  `EnableAdhocServer = True` on both instances and it just works.
- `bEnableWlan` ("EnableWLAN") must be on or every net call returns errors/-1.

### 11.17 Misc facts picked up en route

- Adhocctl event/state delays (`Core/HLE/NetAdhocCommon.h:35-43`): default delay 10 ms,
  event poll delay 100 ms, `adhocEventDelay = 2000000 // "2000000 on real PSP ?"` — expect
  **up to ~2 s between Connect and the CONNECT event** even on PPSSPP; our
  session-establish timeout must exceed that.
- PPSSPP enforces a **minimum socket timeout** (`MinTimeout` config, µs floor applied to
  blocking net ops, `sceNet.cpp:611`, `proAdhoc.cpp:2012` "Override timeout for high
  latency multiplayer").
- `sceNetAdhocctlGetState` polling is cheap and reliable; the GetState/handler pair agree
  because both are driven by the same `__NetTriggerCallbacks` state application.
