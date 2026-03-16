# Repo Notes

## Local Build/Test

- Prefer `./cmake-build-debug/dbms/gtests_dbms` for targeted local gtest runs.
- When building `gtests_dbms` locally, reuse a Codex-owned ccache directory to avoid permission issues and keep later incremental builds fast:
  `env CCACHE_DIR=$HOME/.codex/memories/ccache CCACHE_TEMPDIR=/tmp/ccache-tmp cmake --build cmake-build-debug --target gtests_dbms --parallel 16`
- Reuse the same `CCACHE_DIR` for later incremental `gtests_dbms` builds.
- After modifying any C++ source that is linked into `gtests_dbms`, always rebuild `gtests_dbms` before running tests. Running `./cmake-build-debug/dbms/gtests_dbms` directly does not pick up source edits until the binary is rebuilt.
- After adding a new `gtest*.cpp` or `bench*.cpp` source file, run `cmake -S . -B cmake-build-debug` once before rebuilding. The test targets collect sources through CMake globbing, and a plain `cmake --build` may miss the new file until CMake is regenerated.
- After modifying proto files under `contrib/tipb/proto/*.proto` or `contrib/kvproto/proto/*.proto`, regenerate the Go bindings in the corresponding submodule; changing the proto alone is not enough.
- For `tipb`, prefer:
  `cd contrib/tipb && env GOMODCACHE=$HOME/.codex/memories/go-mod-cache GOCACHE=$HOME/.codex/memories/go-build-cache GOPATH=$HOME/.codex/memories/go GOBIN=$HOME/.codex/memories/go/bin PATH=$HOME/.codex/memories/go/bin:$PATH make go`
- For `kvproto`, prefer:
  `cd contrib/kvproto && env GOMODCACHE=$HOME/.codex/memories/go-mod-cache GOCACHE=$HOME/.codex/memories/go-build-cache GOPATH=$HOME/.codex/memories/go make go`
