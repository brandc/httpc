![Programming Language](https://img.shields.io/badge/C-Programming%20Language-brightgreen)
![Zero Clause BSD License](https://img.shields.io/badge/License-BSD%20Zero%20Clause-green)

# httpc
High performance http server, written to handle over 10K clients at a time.
This is a compliant HTTP/1.0 server.
It supports basic caching by date.
Only the GET verb has been implemented.

# Usage
Simply run
```
./httpc
```
NOTE: Ensure that you have changed the root directory with something like chroot first. File paths for this program start from the root directory.

# Building
## Requirements:
- GNU C Compiler
- Linux Environment

## Process
Build process
```
make all
```

# License
All code and files in this repository are licensed under the 0-BSD License
