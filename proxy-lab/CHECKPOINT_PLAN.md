# Proxy Lab Checkpoint Plan

Scope: finish the checkpoint only. That means passing the `A` and `B` test groups in `driver.sh check`.

## Goal

Build a working single-threaded HTTP proxy that:

- accepts client connections
- parses `GET` requests
- forwards the request to the origin server
- relays the response back to the client
- handles basic errors without crashing
- formats outbound headers correctly for the strict tests

## Non-Goals For Checkpoint

- no cache
- no concurrency / threading
- no eviction policy
- no performance tuning beyond correctness

## Step-by-Step Implementation Plan

1. Replace the starter `main()` in `proxy.c` with a real proxy server loop.
   - parse the listening port from `argv`
   - ignore `SIGPIPE`
   - open a listening socket
   - accept client connections in a loop
   - handle one client at a time

2. Add a per-connection handler.
   - create `handle_client(int clientfd)`
   - keep all request parsing, forwarding, and cleanup inside this function
   - close both client and server sockets on every exit path

3. Parse the request line.
   - read the first line from the client
   - extract method, URI, and HTTP version
   - accept only `GET`
   - reject malformed request lines cleanly

4. Parse the URI into `hostname`, `port`, and `path`.
   - support `http://host[:port]/path`
   - default port to `80` when omitted
   - default path to `/` when omitted
   - do not assume the URI already contains a path

5. Read and classify client request headers.
   - consume all headers until the empty line
   - remember the `Host` header if needed
   - ignore client-provided `Connection`, `Proxy-Connection`, and `User-Agent`
   - ignore unrelated headers for checkpoint

6. Rebuild the outbound request in proxy-safe form.
   - request line should use origin-form, e.g. `GET /path HTTP/1.0`
   - always send `Host`
   - always send the lab `User-Agent`
   - always send `Connection: close`
   - always send `Proxy-Connection: close`
   - terminate headers with a blank line

7. Connect to the origin server.
   - resolve the destination with `getaddrinfo` or the CS:APP socket helpers
   - open a client socket to the target host and port
   - if connection fails, return a proxy error response and keep the process alive

8. Forward the origin response to the browser.
   - read bytes from the server socket in a loop
   - write bytes to the client socket immediately
   - do not treat the response body as a C string
   - preserve binary responses

9. Add robust error handling.
   - invalid request line
   - unsupported method
   - DNS or connect failure
   - short reads or write failures
   - client disconnects
   - any failure should only affect the current request, not the whole process

10. Keep the code organized for the later checkpoint/final split.
    - keep parsing logic in helper functions
    - keep request rebuilding separate from I/O
    - do not mix cache-related code into checkpoint work

## What To Verify During Coding

- `A` tests: basic request forwarding works
- `B` tests: proxy survives errors and strict header checks
- proxy does not crash on `SIGPIPE`
- large responses are streamed correctly
- binary responses are preserved
- invalid requests produce a handled error path

## Validation

Run these from the proxy lab directory after each meaningful change:

```bash
make clean
make proxy
./driver.sh check
```

If you want a faster local smoke test before the full checkpoint run:

```bash
make proxy
./proxy <port>
```

Then drive it with a browser or a minimal curl request through the proxy.

## Success Criteria

- `make proxy` succeeds
- `./driver.sh check` reports all `A` and `B` tests passed
- the process stays alive after malformed input or broken connections
- request formatting matches the strict tests

