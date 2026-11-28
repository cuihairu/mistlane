# Third-party dependencies

This source release obtains its build dependencies through vcpkg. Two
protocol-conformance test-only source snapshots are vendored in the package
and listed below; they are compiled only into the `mistlane-sproto-probe`
test target and are not linked into the Mistlane server (see
docs/dependencies.md for the pinned upstream commits).

| Dependency | License | Upstream |
|---|---|---|
| Boost and its modules | Boost Software License 1.0 | https://www.boost.org/ |
| Drogon | MIT AND BSD-3-Clause | https://github.com/drogonframework/drogon |
| Trantor | BSD-2-Clause | https://github.com/an-tao/trantor |
| libcurl | curl AND ISC AND BSD-3-Clause | https://curl.se/ |
| vcpkg port recipes | MIT | https://github.com/microsoft/vcpkg |
| Lua | MIT | https://www.lua.org/license.html |
| OpenSSL 3.x | Apache-2.0 | https://www.openssl-library.org/ |
| VitePress / Vite | MIT | https://vitepress.dev/ / https://vite.dev/ |
| pnpm | MIT | https://pnpm.io/ |
| sproto (vendored, test-only) | MIT | https://github.com/cloudwu/sproto |
| LPeg (vendored, test-only) | MIT | https://www.inf.puc-rio.br/~roberto/lpeg/ |

The vendored snapshots live under `third_party/sproto/` and
`third_party/lpeg/`; their copied license texts are
`licenses/sproto-MIT.txt` and `licenses/lpeg-MIT.txt`.

vcpkg installs each port's license/copyright and software bill of materials under
`vcpkg_installed/<triplet>/share/<port>/`. Preserve the applicable license and
notice material when distributing compiled binaries and their dependencies.
Mistlane project code is covered by the repository's Apache-2.0 LICENSE.
Adapted vcpkg port files retain their upstream MIT license in
`ports/LICENSE.txt`. Source provenance is listed in NOTICE; preserve applicable
third-party license texts and notices when redistributing.
