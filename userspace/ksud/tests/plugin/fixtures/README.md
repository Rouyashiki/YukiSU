# Upstream compatibility fixtures

These files retain the upstream source bytes; they are not rewritten for YukiSU.

- `folkpatch`: [FolkPatch boot-script](https://github.com/LyraVoid/FolkPatch/tree/0f3f42ba02d6441dbbeb247f34c0f5eabc14e393/examples/plugins/boot-script).
- `folkpatch-cache`: [FolkPatch cache-cleaner](https://github.com/LyraVoid/FolkPatch/tree/0f3f42ba02d6441dbbeb247f34c0f5eabc14e393/examples/plugins/cache-cleaner).
  Both examples declare MIT in their original manifests; authorship remains FolkPatch.
- `finasu`: the first JSON and Lua code blocks in LyraVoid's
  [minimal plugin example](https://github.com/LyraVoid/FinaSU/blob/53fcde6595a077f7ab2554277c33d08a6e1185a8/website/docs/developer/lua-plugin.md).
  The example manifest declares MIT. The containing documentation is licensed
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/) by LyraVoid.
  The code blocks were extracted without changes; their surrounding prose is not included.

The cache-cleaner runs with an isolated fake `sh` on PATH: cleanup requests are
recorded without executing them. Its file/configuration APIs and daemon remain
the production implementations. The FinaSU package includes an empty `data/`
directory for its greeting output. The boot-script fixture is an expected
Lua 5.5 syntax failure because it assigns to a generic-for control variable.

SHA-256:

```text
fe8ead2e5dd841711eb7ede2e8a1da7d1e086b38ea52c6f530a7e78172187d1d  folkpatch/main.lua
4dde23d027204dd53ef68bdc232f4aafda931e52606b165eb2d93cbf63ff45b3  folkpatch/plugin.json
b9b69e317f54a2f8a05c49ff1240064caba0485484e1ee74a5ad21d0403cb6ef  folkpatch-cache/main.lua
cbdecb0465cc55e793e7648a786215d41f58b26da2b19c17e81edf8a4d24db10  folkpatch-cache/plugin.json
7510e3b2ed781a6154792a1444bbdb67080f50cc3a767c8383b319f116df80d1  finasu/plugin.json
2a701bfacbfbd07a9e920fa1737eff2efc2faa6e43ebe58b02f889c1c2c3b1e5  finasu/main.lua
```
