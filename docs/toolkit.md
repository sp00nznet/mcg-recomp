# The toolkit

The lifter, the PE helpers and the `recomp32` runtime pieces come from
[pcrecomp](https://github.com/sp00nznet/pcrecomp), referenced and never
vendored. The Python steps find it through `PCRECOMP` (else `..\tools`, a
checkout beside this repository; `Setup.cmd` and the installer set it to
`..\pcrecomp` when that exists), CMake through `-DPCRECOMP=` (same default).
The lifter and `recomp_types.h` are two halves of one contract, so both must
come from the same checkout: `run_pipeline.py` copies the header next to the
generated C for exactly that reason.

## Version

Built and run against **pcrecomp `main` at `8f4aa77`** (2026-10-11). No
unmerged toolkit change is needed: every lifter fix this game found has landed
on `main` (see [bringup.md](bringup.md), "Lifter bugs this game found").

Moving from the October 3 branch this project had been using to `main` changed
nothing in the totals: 6,065 functions, 0 lift errors, 13 files.
