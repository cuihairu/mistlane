# WebSocket `std::bad_weak_ptr` investigation

Status: unresolved; no root cause assigned. Updated 2026-10-19.

## Observed failure

The pre-fix full-suite log `/tmp/mistlane-full-pre-1.log` reports:

```text
7/31 Test  #7: mistlane_transport_websocket ......Subprocess aborted***Exception:   1.63 sec
terminate called after throwing an instance of 'std::bad_weak_ptr'
  what():  bad_weak_ptr
```

In that sample, the websocket fixture had already called `c.node()` and waited
for readiness before connecting (`src/test_cases.cpp`, around lines 687 and
697). The sample therefore does not support a diagnosis that the test connected
before the server was ready.

## Candidate call sites

These uses need a captured stack before attributing the exception:

- `src/web.cpp:44` and `src/web.cpp:50`: `WebChannel::incoming()` and
  `WebChannel::peer_closed()` capture `shared_from_this()` in posted handlers.
- `src/node.cpp:1751` and `src/node.cpp:1760`: websocket send/read callbacks
  capture `Node::shared_from_this()`.

The candidate mechanism is a callback reaching `shared_from_this()` after the
owning object's shared lifetime has ended. This remains a hypothesis, not a
finding; no destructor-order or fixture cause has been established.

## Reproduction and stack-capture attempts

- Before this investigation: 10 breakpoint-isolated attempts, no reproduction
  (reported with the task context).
- One gdb run of `mistlane test --case transport.websocket`, following the
  parent process and breaking on `std::__throw_bad_weak_ptr()`: websocket passed;
  breakpoint not hit, so no stack.
- ASAN build of the `mistlane` target using
  `-fsanitize=address -fno-omit-frame-pointer`: 20 targeted websocket runs;
  19 passed and one (run 2) aborted with `pure virtual method called`, not
  `bad_weak_ptr`. No AddressSanitizer diagnostic was emitted. The abort log is
  `/tmp/mistlane-websocket-asan-2.log`.
- Five serial full-suite runs with `ctest -E mysql`: runs 1–4 passed 31/31;
  run 5 passed the websocket case but failed `coordinator.registry` with
  `member registration not logged`. The per-run logs are
  `/tmp/mistlane-websocket-investigation-full-{1..5}.log`.

None of these attempts captured the target exception or its stack. The
`pure virtual method called` abort is retained as a separate observation; it
does not establish a connection to `bad_weak_ptr`.

## Follow-up condition

If `std::bad_weak_ptr` recurs, preserve the complete failing log and run the
websocket case under gdb with a breakpoint on
`std::__throw_bad_weak_ptr()` (or `catch throw`), keeping the failing process
attached long enough to save `bt full` before termination. Use that stack to
decide whether the cause is a product lifetime defect in `WebChannel`/`Node`
or a test-fixture defect. If it does not recur, keep the issue unresolved and
retain the original log and this attempt record; do not claim a root cause from
the candidate sites alone.
