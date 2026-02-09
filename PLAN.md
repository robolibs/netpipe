# HTTP11 + HTTP2 Implementation Plan

This plan introduces two new protocol modules in netpipe:
- `http11` for HTTP/1.1
- `http2` for HTTP/2

The implementation must maximize reuse of `dp::` types and patterns across all code:
- use `dp::Res<T>` and `dp::Error` for all fallible operations
- use `dp::String`, `dp::Vector`, `dp::Map`, `dp::Optional`, `dp::usize`, `dp::u*` everywhere
- use datapod serialization utilities where wire format helpers are needed
- avoid exceptions for protocol/runtime flow; return typed `dp::Res` errors

Every step below ends with:
1) adding/updating a dedicated test file
2) running tests
3) creating a git commit for only the completed step

---

## Phase 0 - Foundation and module scaffolding

### Step 0.1: Public API scaffolding
- Add umbrella headers:
  - `include/netpipe/protocol/http11.hpp`
  - `include/netpipe/protocol/http2.hpp`
- Add internal module trees:
  - `include/netpipe/protocol/http11/`
  - `include/netpipe/protocol/http2/`
- Add base shared HTTP types (headers, method, status helpers) that both modules can use without leaking transport details.
- Define error categories and helper constructors using `dp::Error` messages that are protocol-specific.

Test file:
- `test/test_http_foundation.cpp` (compile/API smoke coverage for new headers and basic type semantics)

Commit:
- `feat(protocol): scaffold http11/http2 modules and shared http foundation`

### Step 0.2: Transport compatibility layer
- Define explicit adapters to bridge netpipe stream semantics with protocol parsers:
  - framed message mode (existing netpipe stream behavior)
  - raw byte-stream mode abstraction for HTTP/1.1 and HTTP/2 interoperability
- Keep adapter APIs transport-agnostic and `dp::Res`-first.

Test file:
- `test/test_http_transport_adapter.cpp`

Commit:
- `feat(protocol): add stream adapter layer for http protocols`

---

## Phase 1 - HTTP/1.1 (`http11`) implementation

### Step 1.1: Start line + header parsing
- Implement strict parser for:
  - request line (`METHOD SP URI SP HTTP/1.1`)
  - status line (`HTTP/1.1 SP status SP reason`)
  - header lines and folded/invalid header rejection strategy
- Normalize header handling with case-insensitive lookup helpers.

Test file:
- `test/test_http11_parser.cpp`

Commit:
- `feat(http11): add request/status line and header parser`

### Step 1.2: Serialization
- Implement request/response serializers with deterministic header order policy.
- Provide helpers for common content headers.
- Guarantee roundtrip parser<->serializer compatibility for supported cases.

Test file:
- `test/test_http11_serialize.cpp`

Commit:
- `feat(http11): add request and response serialization`

### Step 1.3: Body framing and connection semantics
- Implement body handling for:
  - `Content-Length`
  - `Transfer-Encoding: chunked`
  - empty-body rules by method/status
- Add keep-alive/connection-close behavior model.

Test file:
- `test/test_http11_body.cpp`

Commit:
- `feat(http11): implement content-length and chunked body handling`

### Step 1.4: Client and server session objects
- Add `http11::ClientConnection` and `http11::ServerConnection` stateful helpers.
- Support sequential request/response exchange with robust parse boundaries.
- Map protocol faults to clear `dp::Error` values.

Test file:
- `test/test_http11_connection.cpp`

Commit:
- `feat(http11): add stateful client/server connection helpers`

### Step 1.5: Interop readiness pass
- Add strict/lenient parse mode options.
- Validate compatibility with raw TCP adapter path.
- Document known unsupported features (if any) directly in header docs.

Test file:
- `test/test_http11_interop.cpp`

Commit:
- `feat(http11): add interop modes and compatibility checks`

---

## Phase 2 - HTTP/2 (`http2`) implementation

### Step 2.1: Core wire types and frame codec
- Implement frame definitions and codec for core frame set:
  - DATA, HEADERS, PRIORITY, RST_STREAM, SETTINGS, PUSH_PROMISE, PING, GOAWAY, WINDOW_UPDATE, CONTINUATION
- Add stream id and flag validation per frame type.

Test file:
- `test/test_http2_frame.cpp`

Commit:
- `feat(http2): implement core frame types and codec`

### Step 2.2: HPACK implementation
- Implement HPACK static table support first.
- Add Huffman decode/encode reuse where valid; keep RFC-compliant HPACK behavior separate from QPACK logic.
- Add dynamic table, indexing modes, and size update semantics.

Test file:
- `test/test_http2_hpack.cpp`

Commit:
- `feat(http2): implement hpack static and dynamic tables`

### Step 2.3: Connection preface + SETTINGS state machine
- Implement client/server preface validation.
- Implement SETTINGS exchange and ACK flow.
- Enforce protocol errors for invalid startup sequencing.

Test file:
- `test/test_http2_settings.cpp`

Commit:
- `feat(http2): add preface and settings state machine`

### Step 2.4: Stream lifecycle and header blocks
- Implement stream state transitions (`idle/open/half-closed/closed`).
- Implement HEADERS+CONTINUATION reassembly.
- Parse pseudo-headers and enforce ordering/validity rules.

Test file:
- `test/test_http2_stream.cpp`

Commit:
- `feat(http2): implement stream lifecycle and header reassembly`

### Step 2.5: Flow control and prioritization
- Implement connection and stream windows.
- Implement WINDOW_UPDATE handling and exhaustion checks.
- Implement priority tree model (initial version may use simplified scheduling with documented constraints).

Test file:
- `test/test_http2_flow_control.cpp`

Commit:
- `feat(http2): add flow control and priority scheduling`

### Step 2.6: Error handling and graceful shutdown
- Implement RST_STREAM/GOAWAY behavior and error code mapping.
- Ensure deterministic teardown and in-flight stream outcomes.

Test file:
- `test/test_http2_shutdown.cpp`

Commit:
- `feat(http2): implement stream reset and connection shutdown`

### Step 2.7: TLS/ALPN integration path
- Extend TLS support to advertise/select ALPN values (`h2`, `http/1.1`).
- Add negotiation hooks so transport selection can instantiate `http11` or `http2` session objects.

Test file:
- `test/test_http_alpn.cpp`

Commit:
- `feat(tls): add alpn negotiation for http11 and http2`

---

## Phase 3 - Unified user-facing API and examples

### Step 3.1: Protocol selector facade
- Add a high-level selector API that chooses `http11` or `http2` based on config and negotiated capabilities.
- Keep API shape aligned with existing `netpipe::http3` ergonomics where practical.

Test file:
- `test/test_http_selector.cpp`

Commit:
- `feat(protocol): add unified http protocol selector`

### Step 3.2: Examples
- Add:
  - `examples/example_http11.cpp`
  - `examples/example_http2.cpp`
- Include basic request/response, streaming body, and error-path demonstrations.

Test file:
- `test/test_http_examples_compile.cpp`

Commit:
- `examples: add http11 and http2 usage demos`

---

## Phase 4 - Hardening and release readiness

### Step 4.1: Fuzz/property testing targets
- Add parser/frame codec fuzz-friendly entrypoints.
- Add corpus tests for malformed packets and boundary lengths.

Test file:
- `test/test_http_fuzz_regression.cpp`

Commit:
- `test(http): add fuzz regression corpus and malformed input coverage`

### Step 4.2: Performance and memory budget checks
- Add benchmarks for:
  - HTTP/1.1 parse/serialize throughput
  - HTTP/2 frame+HPACK throughput
- Add guards for table/window memory growth.

Test file:
- `test/test_http_perf_sanity.cpp`

Commit:
- `perf(http): add baseline throughput and memory sanity checks`

### Step 4.3: Final conformance sweep
- Consolidate RFC requirement checklist in code comments/docstrings near implementation points.
- Run full test matrix and close gaps.

Test file:
- `test/test_http_conformance_smoke.cpp`

Commit:
- `chore(http): conformance sweep and final protocol stabilization`

---

## Definition of Done (for each step)

- All new fallible paths return `dp::Res<T>` and never throw.
- No raw STL types in protocol public APIs where `dp::` equivalents exist.
- Dedicated step test file exists and passes.
- Commit created with step-specific message.
- `make config && make build && make test` passes before moving to next step.
