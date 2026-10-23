# Motor C runtime shim

This Rust crate is part of mlibc's Motor OS port. It builds
`libmoto_rt_cabi.a`, the C ABI over the published `moto-rt` and `moto-sys`
crates, and supplies `moto_rt.h`. It also owns emulated TLS and C++
thread-local destructor registration. No Motor OS source checkout is needed.

Use the Motor Rust toolchain selected by the toolchain builder, with its
`bin` directory on `PATH` or selected through rustup. It must include
`rust-src`. From this crate's directory, fetch the locked registry dependencies
before an offline build:

```sh
cargo fetch --locked
cargo build --locked --offline --release --target x86_64-unknown-motor \
  -Zbuild-std=core,alloc
```

Build this crate before mlibc. Install the archive from
`target/x86_64-unknown-motor/release/` into the Motor sysroot's
`devtools/llvm/lib`, and this directory's `moto_rt.h` into
`devtools/llvm/include`. The toolchain builder may set `CARGO_TARGET_DIR`.
Both files must come from the same mlibc source revision. The build checks
the header's runtime version against the actual `moto-rt` dependency.

Keep the shim as a separate archive. Clang's Motor linker recipe places it
before the C++ runtime libraries in the library group, so its TLS destructor
implementation is selected. Exclude compiler-rt's emulated-TLS implementation.
The shim must contain no unwinder references and no strong `motor_start`,
`memcpy`, `memmove`, `memset`, or `memcmp` definitions. Its explicit abort
profiles and `core,alloc` build preserve the runtime used by mixed Rust/C/C++
programs.

Update `Cargo.lock` deliberately when changing dependencies. Validate the
standalone Motor build and the toolchain's C/C++ and mixed-language runtime
tests before advancing the mlibc revision consumed by Motor OS.
