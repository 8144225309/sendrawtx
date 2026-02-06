# Contributing to sendrawtx

## Building

**Prerequisites:**
- Linux (Ubuntu 22.04+) or macOS (14+)
- GCC or Clang
- pkg-config
- libevent, OpenSSL, nghttp2 (dev packages)

Install dependencies:

```bash
make deps
```

Build:

```bash
make clean && make
```

The build uses `-Wall -Wextra -Werror`. If it doesn't compile clean, it doesn't ship.

## Testing

Before submitting a PR, run both:

```bash
make test                        # unit tests
./tests/test_integration.sh      # end-to-end HTTP tests
```

For memory-sensitive changes, also run with AddressSanitizer:

```bash
make CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" clean all
./tests/test_integration.sh
```

Or Valgrind:

```bash
make valgrind
```

CI runs the full build matrix (Ubuntu 22/24, macOS 14/15 × gcc/clang), sanitizers, and cppcheck. Your PR needs to pass all of them.

See [TESTING.md](TESTING.md) for the full testing strategy.

## Code Style

- 4-space indentation, no tabs
- 1TBS brace style (opening brace on same line)
- `snake_case` for functions and variables
- `PascalCase` for types and structs
- `UPPER_SNAKE_CASE` for macros and constants
- Keep functions short. If it scrolls, split it.

Example:

```c
typedef struct {
    int    request_count;
    char  *client_ip;
} ConnectionState;

#define MAX_HEADER_SIZE 8192

static int handle_request(ConnectionState *conn) {
    if (!conn) {
        return -1;
    }
    // ...
}
```

## Submitting a PR

1. Fork the repo
2. Create a feature branch off `main`
3. Make your changes
4. Run `make clean && make` — zero warnings
5. Run `make test` and `./tests/test_integration.sh` — all pass
6. Push your branch and open a PR

### What makes a good PR

- Solves one thing. Don't bundle unrelated changes.
- Includes tests for new functionality.
- No compiler warnings with `-Wall -Wextra -Werror`.
- Valgrind/ASan clean — no leaks, no undefined behavior.
- Updates docs if you changed config options or user-facing behavior.

### What to update

- New config options → add to `config.ini.example` with inline docs
- New test categories → update `TESTING.md`
- User-facing changes → update `README.md`

## Questions?

Open an issue. There are no dumb questions about C memory management.
