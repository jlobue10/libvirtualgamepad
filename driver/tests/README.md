# Portable driver contracts

Run without installing a driver:

```sh
cmake -S driver/tests -B build/contract-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build/contract-tests --parallel 2
ctest --test-dir build/contract-tests --output-on-failure --parallel 2
```

`vhf_lifecycle_contract` includes the complete production `driver.cpp` and links
the actual profile, report pump, and PID implementations. Only this target uses
the WDF/VHF boundary models in `support/vhf` on Windows. On other platforms, the
descriptor and identity tests also use its minimal Windows ABI declarations.
The real driver and installed-device probes never use those headers.

The lifecycle contract checks:

- Report callbacks inside `VhfStart`, including callbacks on another thread;
  initialized Starting slots service reports while client feedback stays private.
- All five supported profiles, malformed reports, and callback admission across
  Empty/Starting/Active/Stopping, attached/detached, and device-stopping states.
  Reserved generic/PID branches are set up directly for admission tests; public
  creation must continue to reject those profiles and Xbox 360.
- Switch handshake queueing before/after readiness, one submission per credit,
  retained packet/payload lifetime, synchronous readiness and completion reentry,
  and submission failure without manufactured readiness.
- Target-open, missing-file, create and start failures; defensive cancellation at
  adoption; exactly one synchronous deletion and storage reclamation after drain.
- Foreign-owner isolation, owner close, hardware release/restart, target cleanup,
  idempotent teardown, and real mutex serialization of close/release/cleanup
  against an in-progress start.

Models enforce nonrecursive wait locks, no state lock across VHF calls, and no
lifetime lock in callbacks. Unused WDF setup/request routines fail closed.
The Steam Controller keep-alive worker's user-mode primitives (waitable timer,
stop event, worker thread) also fail closed in `support/vhf/windows.h`: the
fixture never creates those handles, so no worker runs here and the keep-alive
scheduling is covered by `test_sc26_keepalive.py` instead; only the performance
counter (the report clock) and `SwitchToThread` (the submit drain) are live. VHF
callbacks are deliberately injected during deletion to verify rejection while
storage is retained. Cancellation-state injection tests a defensive branch;
normal lifecycle callers serialize behind the outer lifetime gate.

The modeled contracts follow Microsoft's
[VhfStart](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfstart),
[VhfDelete](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfdelete),
[VhfReadReportSubmit](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfreadreportsubmit),
and [VhfAsyncOperationComplete](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/vhf/nf-vhf-vhfasyncoperationcomplete)
contracts. Callback admission requires a published handle and completely
initialized storage before calling `VhfStart`. Teardown closes admission and
detaches the handle before `VhfDelete(..., TRUE)`, and retains report storage
through its return.

These tests do not reproduce the Windows scheduler, WDF object deletion, PnP,
IRQL, Windows wide-character ABI, HID enumeration, signing, or kernel behavior.
They do not establish reWASD recognition. A WDK build and Windows runtime
qualification remain separate checks. Installed-device probes are manual and
are not part of the portable suite.
