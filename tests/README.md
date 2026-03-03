# Overview

* `smoke_tests/` contains a handful of small test cases, which mostly exercise
  Tenjin's support for various build system setups. These mostly treat the
  translated code as a black box, and only test the surrounding infrastructure.
* `snapshotted/` contains a handful of tests which focus on the details of
  translated code, and include expected translation results.
* `10j_pytest_fixtures.py` contains helpers, and `conftest.py` exposes it to `pytest`.

# Rationale

`c2rust` has its own snapshot tests. We initially reused their `insta`-based
harness but eventually came to want to test the combination of
`c2rust translate` with our subsequent improvement passes.
