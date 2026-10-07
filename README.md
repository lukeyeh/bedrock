# bedrock

A set of C++ libraries that I find useful.

## The libraries

Each directory is a library and a Bazel package. Dependencies point
downwards only.

```
discord/          a Discord bot's client
├─ websocket/     WebSocket connections
│  └─ http/       HTTP/1.1, as client and as server
│     └─ net/     the event loop; byte streams: TCP, TLS, buffered reading
│        └─ os/   the kernel: sockets, io_uring, epoll, child processes
│           └─ async/   Task, Sequence, Awaitable, TaskScope
└─ json/          JSON values
sqlite/           SQLite databases
testing/          the main function of every test
```

`discord/` also uses `http/`, `net/` and `async/` directly, and so on down:
each library may use any below it on its branch.

### `async/`: coroutines with names

The one place that touches the C++ coroutine machinery (`promise_type`, `await_suspend`, `std::coroutine_handle`), so that everything else is written with `Task<T>`, `co_await` and `co_return` alone.

| Header | What it is for |
| --- | --- |
| `async/awaitable.h` | How to make something a task can wait for. |
| `async/sequence.h` | Sequence<T>: the return type of an asynchronous function that produces many Ts, one at a time. |
| `async/status_macros.h` | ABSL_RETURN_IF_ERROR and ABSL_ASSIGN_OR_RETURN for asynchronous functions. |
| `async/task.h` | Task<T>: the return type of an asynchronous function that produces a T. |
| `async/task_scope.h` | TaskScope: running tasks that nobody awaits, and waiting for several at once with `Join`. |

### `os/`: the kernel in C++ terms

The only code that makes system calls. Sockets are classes that own their descriptor, choices are `enum class`, failures are `absl::Status`, and asynchronous I/O is an `IoDriver` with two interchangeable backends, io_uring and epoll. `os::RunProcess` runs another program without blocking the thread.

| Header | What it is for |
| --- | --- |
| `os/epoll_backend.h` | Internal to //os: the epoll way of carrying out operations. |
| `os/io.h` | The kernel's asynchronous network I/O, as C++. |
| `os/io_uring_backend.h` | Internal to //os: the io_uring way of carrying out operations. |
| `os/process.h` | Other programs, run as child processes: `RunProcess` runs one to its end, and a `Process` is talked to while it runs. |
| `os/random.h` | Random bytes from the kernel: unpredictable to anyone, so fit for things that are secret because they cannot be guessed, such as a session token. |
| `os/socket.h` | Network sockets as C++ objects. Wraps the kernel's socket interface so that callers deal in classes, enums and absl::Status rather than file descriptors, option constants and error numbers. |

### `net/`: an event loop and byte streams

`EventLoop` runs tasks on one thread. `net::Stream` is a byte stream to another machine, plain or TLS, with a deadline on every read; `net::Reader` buffers one for protocols to parse.

| Header | What it is for |
| --- | --- |
| `net/event_loop.h` | The event loop that runs asynchronous functions (see async/task.h) on a thread. |
| `net/process.h` | Another program as a `Stream`: what is written is its standard input, what is read is its standard output. |
| `net/reader.h` | Reads a Stream in the units protocols are made of (lines, fixed-size blocks) rather than in whatever pieces the network delivers. |
| `net/stream.h` | Reliable byte streams to other machines. A `Stream` is what protocols are written against; `Dial` produces one for an address, taking care of name resolution, TCP and (when asked) TLS, so that callers never see which kind they hold. |
| `net/tls.h` | TLS as a layer over any Stream: hand in a connected stream, get back one that encrypts everything written to it and has verified who is on the other end. Most code wants `net::Dial`, which applies this for you. |
| `net/url.h` | Splits a URL into the two things a client needs: where to connect, and what to ask for once connected. |

### `http/`: HTTP/1.1, as client and as server

`http::Serve` has a handler answer every request that arrives on a listener, with `http/form.h` and `http/cookie.h` for what requests carry. `http::Client::Send` is a coroutine that sends a request and returns the response. `Open` returns the head and leaves the body to be read as it arrives, and `http::EventReader` reads such a body as server-sent events. `http::FakeClient` answers from a queue and records what it was sent, for testing code that makes requests.

| Header | What it is for |
| --- | --- |
| `http/client.h` | An HTTP client: give it a request, get back the server's response. |
| `http/cookie.h` | Cookies: small named values a server asks a browser to keep and send back with every later request, which is how a server recognises someone who has signed in. |
| `http/event_stream.h` | Server-sent events: a response body that is a series of small messages, each sent when the server has something to say. |
| `http/form.h` | Names and values written the way HTML forms send them: the body of a posted form, and the query of a URL. |
| `http/fake_client.h` | An http::Client for tests: it records the requests it is sent and answers them from a queue, so code that calls a web API can be tested without one. |
| `http/head.h` | The part of an HTTP/1.1 message that comes before the body: a start line and headers. Requests, responses and the WebSocket handshake all begin with one, and this is the only place that knows how it is written. |
| `http/server.h` | An HTTP server: hand it a listener and a handler, and it has the handler answer every request that arrives. |

### `websocket/`: WebSocket connections (RFC 6455)

`websocket::Connect` opens a connection whose `Receive` and `Send` are coroutines. `websocket::FakeServer` plays the other side from a script, for testing code that talks over one.

| Header | What it is for |
| --- | --- |
| `websocket/fake_server.h` | A scripted WebSocket server for tests. The test says, in order, what the server does (send a message, go quiet, drop the connection, close it), and the client under test, given `connector()`, experiences exactly that, with no network and no waiting. What the client sent is kept for inspection. |
| `websocket/websocket.h` | WebSocket connections (RFC 6455): a long-lived, two-way channel of whole messages. |

### `json/`: JSON values

`json::Value` with lenient readers, `json::Parse` and `json::Serialize`.

| Header | What it is for |
| --- | --- |
| `json/json.h` | JSON documents: parse text into a `Value`, read it, build one, and turn it back into text. |

### `sqlite/`: SQLite databases

`sqlite::Database` owns the connection, keeps compiled statements, binds typed parameters, runs transactions from a callback.

| Header | What it is for |
| --- | --- |
| `sqlite/database.h` | SQLite databases in C++ terms: open one, run SQL with bound parameters, get rows back. This is the only package that touches the SQLite C API; statements are prepared, stepped and finalised inside it, and every failure is an absl::Status carrying SQLite's own explanation. |

### `discord/`: a Discord bot's client

`discord::Client` connects a bot, keeps its gateway session alive (heartbeats, resuming, reconnecting), hands over events one at a time, and makes the REST calls a bot needs: messages, reactions, slash commands.

| Header | What it is for |
| --- | --- |
| `discord/client.h` | A Discord bot's connection to Discord. |
| `discord/gateway.h` | Internal to //discord: the Discord gateway, the WebSocket over which Discord tells a bot what is happening. |
| `discord/model.h` | The things a Discord bot deals in: users, channels, messages, and the events that tell it something happened. |
| `discord/rest.h` | Internal to //discord: Discord's HTTP API, through which a bot acts. |
| `discord/wire.h` | Internal to //discord: how Discord's objects are written in JSON. The only place that knows their field names. |

### `testing/`: the main function of a test binary

One `_test.cc` holds both GoogleTest tests and Google Benchmark benchmarks. Linked against this main, the binary runs the tests by default and the benchmarks when given a `--benchmark` flag.

`testing/main.cc` says how it is used, and `testing/main_test.cc` is a test file written the way it expects.

## A taste

```cpp
#include <memory>
#include <string>
#include <utility>

#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/status_macros.h"
#include "absl/status/statusor.h"
#include "async/status_macros.h"
#include "async/task.h"
#include "http/client.h"
#include "net/event_loop.h"

// Fetches a page. Suspends, rather than blocks, while the network is slow.
Task<absl::StatusOr<std::string>> Fetch(http::Client& client) {
  CO_ASSIGN_OR_RETURN(http::Response response,
                      co_await client.Send(http::Request{
                          .method = http::Method::kGet,
                          .url = "https://example.com/",
                      }));

  co_return std::move(response.body);
}

absl::Status Main() {
  ABSL_ASSIGN_OR_RETURN(EventLoop loop, EventLoop::Create());
  const std::unique_ptr<http::Client> client = http::NewClient();

  // Run is the bridge from ordinary code: it returns what the task returns.
  ABSL_ASSIGN_OR_RETURN(const std::string page, loop.Run(Fetch(*client)));
  LOG(INFO) << page.size() << " bytes";
  return absl::OkStatus();
}
```

[bedrock-skill](https://github.com/lukeyeh/bedrock-skill) teaches a coding
agent the libraries in more depth. [gm_bot_cc](https://github.com/lukeyeh/gm_bot_cc)
is a whole program built on them.

## Using it in another project

The project is expected to be set up as
[nix-bazel-cpp-skill](https://github.com/lukeyeh/nix-bazel-cpp-skill) lays
out: Nix for the toolchain and libraries, Bazel with rules_nixpkgs for
building. Bedrock then arrives the way the third-party libraries do, as a Nix
package that Bazel sees as an external repository.

1. In `flake.nix`, add bedrock as an input that is not a flake, so that
   `flake.lock` pins a revision of it:

   ```nix
   bedrock = {
     url = "github:lukeyeh/bedrock";
     flake = false;
   };
   ```

2. In `nix/deps.nix`, fetch that revision and build it with the project's own
   compiler and libraries, so that the whole program is built one way:

   ```nix
   pkgs:
   let
     stdenv = pkgs.llvmPackages_21.stdenv;
     abseil-cpp = pkgs.abseil-cpp.override { cxxStandard = "20"; };

     lock = builtins.fromJSON (builtins.readFile ../flake.lock);
     bedrock-source =
       let
         locked = lock.nodes.${lock.nodes.root.inputs.bedrock}.locked;
       in
       builtins.fetchTarball {
         url = "https://github.com/${locked.owner}/${locked.repo}/archive/${locked.rev}.tar.gz";
         sha256 = locked.narHash;
       };
   in
   {
     inherit abseil-cpp;
     liburing = pkgs.liburing;
     openssl = pkgs.openssl;
     sqlite = pkgs.sqlite;
     gtest = pkgs.gtest;
     gbenchmark = pkgs.gbenchmark;

     bedrock = pkgs.callPackage "${bedrock-source}/nix/package.nix" {
       inherit stdenv abseil-cpp;
       liburing = pkgs.liburing;
       openssl = pkgs.openssl;
       sqlite = pkgs.sqlite;
       gtest = pkgs.gtest;
       gbenchmark = pkgs.gbenchmark;
     };
   }
   ```

3. In `MODULE.bazel`, present it to Bazel. No BUILD file is given, because
   the package brings its own:

   ```starlark
   nix_pkg.file(
       name = "bedrock",
       attr = "bedrock",
       file = "//nix:bazel.nix",
       file_deps = [
           "//:flake.lock",
           "//nix:deps.nix",
       ],
       repo = "@nixpkgs",
   )
   use_repo(nix_pkg, "abseil-cpp", "bedrock", "gbenchmark", "gtest", "liburing", "openssl", "sqlite")
   ```

   The root `BUILD` needs `exports_files(["flake.lock"])`. The third-party
   libraries have to be imported under the names above, each as one
   `cc_library` of the same name, which is what bedrock's BUILD file refers
   to. This repository's `MODULE.bazel` has those blocks to copy.

4. Depend on a library as `"@bedrock//:net"`, `"@bedrock//:discord"` and so
   on, one target per directory, and include its headers by their full path:
   `#include "net/stream.h"`. Tests link `"@bedrock//:testing_main"` along
   with `"@gtest"` and `"@gbenchmark"`.

5. Build with the same flags (`.bazelrc` here): `-std=c++20 -fno-exceptions
   -Wno-coroutine-missing-unhandled-exception`, with `--dynamic_mode=off`.
   Copy its `test:epoll` line too, to be able to run the project's tests on
   the epoll backend with `--config=epoll`.

`nix flake update bedrock` moves the project to bedrock's latest revision.
For a Nix package of the project itself, `bedrock` as a build input brings
the headers, the static libraries (`-lnet -los -lasync`, each before the
ones it uses) and the third-party libraries with it.

## Working on it

```
nix develop                        # or direnv allow
bazel test //...                   # every test
bazel test --config=epoll //...    # the same on epoll instead of io_uring
bazel run -c opt //net:stream_test -- --benchmark_filter=all   # one file's benchmarks
bazel run :compile_commands        # compile_commands.json, for clangd and clang-tidy
nix build                          # the package other projects get
```

Format with `clang-format -i */*.cc */*.h` and `buildifier -r .`; lint with
`clang-tidy */*.cc`. Start clangd with `--query-driver=/**/*` so that it can
find Nix's system headers.

`nix/package.nix` installs a static library per directory, the headers, and
`nix/installed.BUILD`, which is the BUILD file other projects see. A new
library or a new dependency between libraries has to be added to both.
