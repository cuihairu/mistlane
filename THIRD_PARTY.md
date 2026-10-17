# Third-party dependencies

This source release obtains dependencies through vcpkg. No vendored dependency
source code or binary is included in this package.

| Dependency | License | Upstream |
|---|---|---|
| Boost and its modules | Boost Software License 1.0 | https://www.boost.org/ |
| Lua | MIT | https://www.lua.org/license.html |
| OpenSSL 3.x | Apache-2.0 | https://www.openssl-library.org/ |
| VitePress / Vite | MIT | https://vitepress.dev/ / https://vite.dev/ |
| pnpm | MIT | https://pnpm.io/ |

vcpkg installs each port's license/copyright and software bill of materials under
`vcpkg_installed/<triplet>/share/<port>/`. Preserve the applicable license and
notice material when distributing compiled binaries and their dependencies.
New project code is covered by the MIT LICENSE selected in the independent
repository. Two files derived from the original Apache-2.0 project retain that
license: `include/mistlane/token_bucket.hpp` and `lua/mistlane/min_heap.lua`.
The full Apache-2.0 text is in `licenses/Apache-2.0.txt`. Source provenance is
listed in NOTICE; preserve these markings and notices when redistributing.
