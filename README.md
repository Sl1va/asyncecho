# `asyncecho` - asynchronous TCP echo server ucing `libev`

## Build dependencies

The tool builds and runs successfully

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

For testing purposes standard `nc` tool can be used.

Usage example:
```sh
nc 127.0.0.1 5000
```
