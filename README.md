# `asyncecho` - asynchronous TCP echo server ucing `libev`

## Build dependencies

- `gcc` 9.4.0 or higher
- `libev-dev` 4
- `cmake` 3.16 or higher
- `make` 4.2 or higher

## Dependencies

- `libev4`

## Building

Project can be built using following commands:

```sh
mkdir -p build
cd build
cmake ../
make
```

## Running

The tool does not require any arguments and can be run simply by executing binary.
It listens for any connection on port 5000 and provides echo responses for any message.

For testing purposes standard `nc` tool can be used.

Usage example:
```sh
nc 127.0.0.1 5000
```
