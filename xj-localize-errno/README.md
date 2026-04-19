# Errno Localization

The executable in this subdirectory (`xj-localize-errno`) implements a transform that:
1. Declares a local `_xj_local_errno` integer variable in each function body;
2. Replaces reads and writes of `errno` with reads and writes of `_xj_local_errno`;
3. Replaces calls to library functions `f(...x_i...)` (extern functions defined outside of the target) with
   signature `T f(...T_i...)` to calls to `T _xj_wrap_f(&_xj_local_errno, ...x...)` where 
   the wrapper has signature `T _xj_wrap_f(int *err,...T_i)`;
4. Generates implementations & inserts above-mentioned wrappers

## Preconditions/Assumptions

- The Codehawk-based `errno` analysis checks preconditions for this transform. If the transform fails after
  successfully running the analysis (with 0 open proof obligations), then that is a bug that should be reported.
- The transform assumes that the code is macro-expanded with the expansion of `errno` _blocked_. This can be relaxed at the cost
  of devision a way of identifying `errno` accesses (which has different definitions on different platforms)