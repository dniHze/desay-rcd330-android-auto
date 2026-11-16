# Kanzi 2.8.4 `.KZB` binary container format

Reverse-engineered from `libUICore-kanzi.so` (ELF32 ARM, **not stripped, with
debug_info**) and validated by byte-identical round-trip of five sample files.
All multi-byte integers are **big-endian**. The loader entry points this was
derived from:

| symbol | role |
|---|---|
| `kzuBinaryDirectoryCreateFromMemory` / `...FromSource_internal` | header + directory block parse |
| `kzuBinaryDirectoryReadFolder_internal` | recursive folder/file tree |
| `kzuBinaryReadString` | string record encoding |
| `kzuBinaryFileInfoGet{Size,Type,Flags,Name,Path}` | file-record field semantics |

## Round-trip status (the GO/NO-GO gate) — PASS

`kzb_tool.py` parses each sample into a field-level model and rebuilds it
**byte-for-byte**. Verified with the system `md5` on repacked output:

| sample | size | md5 (orig == repack) |
|---|---|---|
| `01_empty_83k.kzb`     |  83 266 | `ac964f4649b4ff01c48131fc5bfaeeb8` |
| `02_vpanel_97k.kzb`    |  97 105 | `0c256b79f4e447f66963a577cde8bc9d` |
| `03_update_201k.kzb`   | 201 404 | `150389f57caa50e64f600fca6ad86b1d` |
| `04_statusbar_219k.kzb`| 219 864 | `a0ac2c997c7e9046b0db2311be5db479` |
| `appconnect_res.kzb`   | 870 282 | `4afb7eb8c69c058335eb4058a4d6e546` |

The header and the whole directory block are decoded into typed fields and
rebuilt from them; only the trailing **resource-data region** (the opaque
resource blobs) and the XOR-obfuscated UUID bytes are carried as raw spans,
because their *content* is not needed to reproduce the container.

---

## 1. Header  (offset `0x00` .. `header_end`)

```
0x00  char[4]  magic            ".KZB"  = 2e 4b 5a 42
0x04  u32      total_file_size  self-describing (== len(file))
0x08  u32      header_end       offset where the directory block begins;
                                the loader seeks here after reading the name
0x0c  u32      version          107 in every sample. Loader rule:
                                  > 107  -> error (unsupported)
                                  == 107 -> current
                                  <= 106 -> load with a downgrade warning
0x10  u16      schema0          0x0002   ) four fixed "schema" half-words,
0x12  u16      schema1          0x0008   ) identical across all samples;
0x14  u16      schema2          0x0004   ) read as 4x u16 then an
0x16  u16      schema3          0x0102   ) u32 + a skipped u32
0x18  u32      flags_e          1        (drives directory endian flag; see note)
0x1c  u32      field_f          1        (read, then ignored)
0x20  u32      uuid_len         36
0x24  u8[36]   uuid_bytes       project UUID, stored XOR-0x80 obfuscated.
                                The loader reads the length and SKIPS the bytes
                                (never de-obfuscates them). Decoded example:
                                  b36fa4f7-02dd-4573-90b8-bcb6c00832a1 (mazda_g5)
....  s32      type_code        in {10,20,30,40,45,255}; 40 in every sample
....  STRING   name             project name, plain ASCII ("mazda_g5", ...)
....  pad                       seek to header_end (0 bytes in all samples)
```

`header_end` is `0x58` when the name is 8 bytes (`mazda_g5`) and `0x60` when it
is 16 bytes (`vw_mp2017_vpanel`) — it is simply the position just past the name
record, so it tracks the name length. That is why `u32@0x08` is **not** a fixed
constant (it differs between samples) whereas `u32@0x0c` (version 107) is.

Endianness note: the loader fixes big-endian for the fixed header words and for
every `...Int` reader; `flags_e` feeds `directory->flags` (1 or 2). All samples
are big-endian; the tool reads/writes big-endian throughout.

## 2. Directory block  (`header_end` .. `header_end + dir_size`)

```
u32      dir_size        total byte length of this block.
                         data region starts at header_end + dir_size.
u32      num_files       = (file-pointer array length) - 1
                         = (# FILE records in the tree) + (# extra strings)
FOLDER   root            recursive tree, see section 3
u32      num_extra ; num_extra * STRING     reference table (see below)
u32      num_deleted ; num_deleted * STRING deleted-file set (0 in all samples)
pad                      seek to header_end + dir_size
```

* **num_files** is the size of the in-memory file-pointer array minus one
  (index 0 is reserved NULL). `read_folder` fills indices `1..F` with the `F`
  FILE records; the `num_extra` loop then appends the reference strings into the
  same array at `F+1..F+num_extra`. Hence `num_files == F + num_extra`.
  Observed: 618 = 46 + 572; 1540 = 968 + 572.
* **extra strings** are relative resource reference paths such as
  `PropertyTypeLibrary/CameraAspectRatio` — the directory's dependency/reference
  table. (The literal `kzb://…` URLs live *inside* resource blobs, as plain
  ASCII, not in this table.)
* **deleted** is a hash-set of file paths removed by a merge/patch; empty here.

## 3. FOLDER record (recursive)

```
u32      folder_size      byte length of this entire folder record
STRING   name             folder leaf name ("" for root)
u32      num_subfolders ; num_subfolders * FOLDER
u32      num_files      ; num_files * FILE
pad                       seek to (folder_start + folder_size)
```

## 4. FILE record

```
u32      offset   absolute byte offset of the resource blob within the .KZB
u32      size     byte length of the resource blob
s32      type     resource type id (see table below)
u32      flags    bit1(0x2)=cached  bit4(0x10)=used-in-cubemaps
                  bit5(0x20)=image premultiplied-alpha
STRING   name     leaf name
```

The full path of a file is the concatenation of the folder names down to it,
e.g. `/Prefabs/CarPlayLayer_Prefab/CarPlayLayer_Prefab/Scene/Camera`.

Observed `type` ids (empirical, from the decoded trees — indicative, not an
exhaustive authority):

| type | kind |
|---|---|
| 10   | image / PNG (flags `0x20`) |
| 100  | Scene |
| 120  | node / mesh node (Plane, child nodes) |
| 130  | Camera |
| 180  | prefab instance root |
| 510  | mesh (`PrimitiveMeshInternal`) |
| 1000 | light (`PointLight`, `DirectionalLight`) |
| 1120 | material type |
| 1121 | material |
| 2020 | texture |
| 2080 | prefab template placeholder (size 8) |

## 5. STRING record  (`kzuBinaryReadString`)

```
u16  length
  if length == 0xFFFF : NULL string. 2 bytes total, no data, no padding.
  else                : length data bytes (plain bytes; a NUL terminator is
                        added only in memory), then padding so that
                        (2 + length) is rounded up to a 4-byte boundary.
                        pad_len = (-(2 + length)) & 3
```

Strings in the directory (folder/file names, project name, reference paths) are
**plain** (not XOR'd). Only the header UUID is XOR-0x80 obfuscated, and the
loader never decodes it.

## 6. Data region  (`header_end + dir_size` .. EOF)

Raw resource blobs, addressed by `FILE.offset` / `FILE.size`. Each blob begins
with a small internal header that depends on the resource type (for images the
4 bytes `00 00 00 00` precede the real `89 50 4E 47` PNG signature, so
`offset` points 4 bytes before the PNG). These per-type blob internals are
**not** decoded here; they are preserved verbatim, which is sufficient for
byte-identical repacking. Decoding a blob's internals is only required to edit a
resource's *content*, not to clone/retarget nodes.

---

## 7. `kzb_tool.py` usage

```
python3 -I kzb_tool.py dump    samples/appconnect_res.kzb [--files]
python3 -I kzb_tool.py verify  samples/appconnect_res.kzb      # round-trip md5
python3 -I kzb_tool.py selftest samples/*.kzb
```

`parse(data) -> model` and `build(model) -> bytes` are the library API;
`build(parse(x)) == x` for every sample. Always run with `python3 -I` on these
untrusted firmware files.

---

## 8. Blob internals: how resources reference each other (Phase 2)

Decoded from `kzuBinaryDirectoryReadReference`, `kzuBinarySourceGetReferencedFile-
Path`, `kzuBinaryReadProperties_internal`, `kzuBinaryReadProperty_internal`,
`kzuObjectNodeLoadFromKZB_protected`, the mesh/material/texture loaders, and
confirmed against the sample data.

There are **two** ways a resource blob points at another resource:

1. **Index reference** — `kzuBinaryDirectoryReadReference` reads a `u32` and
   returns `source->fileArray[index]`. The file-pointer array is exactly the one
   built during directory load:
   - `[0]` = NULL
   - `[1 .. F]` = embedded files **in loader DFS order** (subfolders first, then
     the folder's own files, recursively — `kzuBinaryDirectoryReadFolder_internal`)
   - `[F+1 .. F+num_extra]` = the `extra` reference strings
   So an index is a `u32` position into that flat list. Index references observed
   inside blobs: **prefab-template → instance-root** (type 2080 blob; `u32` at
   offset 3) and **mesh → material** (type 510 `PrimitiveMeshInternal` blob;
   `u32` at offset 6). Validated: every type-2080 template's offset-3 index
   resolves to its `/Prefabs/<name>/<name>` instance root (e.g. CarPlay → 173);
   every mesh's offset-6 index resolves to a `/Materials/*` file.

2. **Inline URL string** — most cross-resource links (material → texture,
   node → mesh, scene → composer, …) are stored as an inline string, encoded as:
   ```
   [u16 BE length][0x00][`length` ASCII bytes]      (then a 0x00 terminator)
   ```
   (the `u16` length sits 3 bytes before the ASCII; one `0x00` separator byte
   follows it). Two URL spellings appear: `kzb://<project>/<path>` and
   `kzResID<path>`. The `<path>` is the resource's directory path. Example, in
   the CarPlay `Plane` node: `kzb://vw_mp2017/Prefabs/CarPlayLayer_Prefab/
   CarPlayLayer_Prefab/Scene/Plane/PrimitiveMeshInternal`.

**Object-node parent/child structure is the FOLDER hierarchy**, not in-blob
references (`kzuObjectNodeLoadFromKZB_protected` has no child-list read; it reads
the property list, then triggers, then bindings, then a resource dictionary).

**Property records** (inside `kzuBinaryReadProperties`): `[u32 nprops][nprops ×
property][u32 ngroups][ngroups × resource-ref]`. Each property is
`[u32][string property-type NAME][u32][typed value]` — property TYPES are named
inline (ASCII), not indexed, so they do not consume file-array indices.

**Blob layout caveat.** `FILE.size` under-reports a blob's real content: blobs
are laid out with a variable inter-blob gap (observed 4..14+ bytes) and a URL can
run past `offset+size` up to the next blob. The real content therefore runs from
`offset` up to the next blob's `offset`. `kzb_tool.py` clones **gap-bounded**
content (`offset .. next_offset`) and keeps `size` consistent, which reproduces
the layout exactly (idempotent re-parse).

## 9. Prefab clone — implemented (`kzb_tool.py clone-prefab`)

`clone-prefab` deep-copies `/Prefabs/CarPlayLayer_Prefab` (its folder subtree +
the type-2080 template file) into `/Prefabs/AndroidAutoLayer_Prefab`, appended at
the end of `/Prefabs`, then:
- renames every folder/file name `CarPlayLayer_Prefab → AndroidAutoLayer_Prefab`;
- rewrites inline URL strings in the cloned blobs to the clone's own path
  (fixing the `u16` length), so the cloned `Plane` points at the cloned mesh;
- **repoints the cloned mesh's material** (type-510 offset-6 index) to
  `/Materials/TexturedMaterial_Androidauto`;
- rewrites intra-clone index references (template → cloned instance-root, etc.)
  via an old→new index map;
- shifts every existing blob offset by the directory-growth delta, lays the new
  blobs after the existing data region, and recomputes `folder_size`/`dir_size`/
  `num_files`/`total_size` (`recompute_sizes`).

Produced `appconnect_res.aa.kzb`. **Verified (`clone-prefab` + `check`):**
- regression: all 5 originals still repack byte-identical (parse/build untouched);
- the new file re-parses and is **idempotent** (`build(parse(aa)) == aa`);
- every blob offset is in-bounds, blobs do not overlap, `total_size` == file length;
- every inline `kzb://` / `kzResID` URL resolves to an existing file path (the
  4 the checker flags are a checker-side length-mis-read, identical in the
  original — not introduced by the clone);
- every mesh→material index reference in the whole file still resolves to a
  `/Materials/*` file (0 bad), and the cloned mesh resolves to `..._Androidauto`;
- the cloned template resolves to the cloned instance root; the cloned `Plane`
  mesh URL resolves to the cloned mesh.

**Honest limitations / residual risk:**
- Appending inside `/Prefabs` shifts the loader indices of files enumerated after
  it (`/Locales … /State Managers`, the root `/VW_MP2017` node, and the `extra`
  table) by +6. All *cross-resource* links in this format are inline URLs (path-
  based, immune to the shift) and the two index-based link types point only into
  unshifted regions (`/Materials`, earlier `/Prefabs` entries), which is why all
  checks pass. The residual risk is any index-based reference type not covered
  here that points into the shifted range; none was found, but the property
  reader *can* also store a resource reference as an index, so this is asserted
  from the sample evidence, not proven exhaustively.
- **Screen instantiation is NOT implemented.** Making the AndroidAuto tile appear
  requires editing the AppConnect screen's node graph to add a child that
  instantiates the new prefab; that is node-surgery on the screen blob and was
  left out.
- The CarPlay prefab's mesh actually binds `/Materials/PhongMaterial` (index 71),
  **not** `TexturedMaterial_AppCarplay`. The AppCarplay/Androidauto *textured*
  materials are bound at the screen/instance level, not inside the prefab. The
  clone repoints the cloned mesh to `TexturedMaterial_Androidauto` as requested;
  whether that is the binding that drives the visible tile cannot be confirmed
  without the device.
- **Rendering is unverified** — correctness here means structural validity,
  reference resolution and idempotent re-parse only, checked with `kzb_tool.py`.
  It was not run on the target hardware/renderer.

## 10. Original sketch (superseded by §9)

> The pre-implementation plan below is kept for reference.

Present in `appconnect_res.kzb` today (decoded):
* `/Prefabs/CarPlayLayer_Prefab` (type 2080, placeholder) and its instance
  subtree `/Prefabs/CarPlayLayer_Prefab/CarPlayLayer_Prefab/Scene/{Camera,Plane,
  Plane/PrimitiveMeshInternal}`.
* Materials `/Materials/TexturedMaterial_AppCarplay` and the already-present
  **target** `/Materials/TexturedMaterial_Androidauto` (both type 1121).

Clone strategy:

1. **Add folders + FILE records.** Deep-copy the `CarPlayLayer_Prefab` folder
   subtree and the sibling placeholder entry, renaming the leaf folders/files
   `CarPlayLayer_Prefab` -> `AndroidAutoLayer_Prefab`. Insert them into
   `root` under `/Prefabs`. `build()` already recomputes `num_subfolders` /
   `num_files` / folder sizes from the model, but `folder_size` and the parent
   `dir_size` are stored values — they must be **recomputed** for any folder
   whose contents change (the current tool trusts the stored sizes for lossless
   round-trip; a `recompute_sizes()` pass is needed for authoring).

2. **Append the cloned resource blobs.** For each new FILE, append a copy of the
   source blob to the end of the data region and set the new FILE's `offset` to
   its new absolute position (and `size` to the copied length). Because blobs
   are addressed by absolute offset, existing entries are untouched; only the
   new ones point into the appended area. Then bump `total_file_size` (`u32@0x04`)
   to the new length.

3. **Repoint material references.** The nodes that referenced
   `TexturedMaterial_AppCarplay` must instead reference
   `TexturedMaterial_Androidauto`. Material binding is carried **inside** the
   node/material blob as a resource reference — either an index into the
   directory reference table (the `extra` strings) or an inline path string. So
   this step *does* require decoding the referencing blob's property section
   (`kzuBinaryResourceLoadFromKZB` / the material-reference property reader) to
   find and rewrite the reference from the CarPlay material id/path to the
   Androidauto one. Since the Androidauto material already exists, no new
   material blob is needed — only the reference target changes.

4. **Register the new prefab** wherever the CarPlay prefab is referenced by the
   screen/layer graph (same reference-rewrite mechanism as step 3), if the new
   layer is to be instantiated.

5. **Rebuild** with `build(model)` and reload on target.

Steps 1–2 and the size recompute are pure container edits fully supported by the
decoded model. Step 3/4 (reference rewriting) is the only part that needs the
per-type blob property layout, which is the natural next RE target
(`kzuBinaryDirectoryReadReference`, `kzuPropertyType*LoadFromKzb`,
`kzuBinaryResourceLoadFromKZB`).
