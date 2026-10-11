# Vendored gtkpyte

The terminal widget and its emulation core, copied unmodified from
https://github.com/scottpeterman/gtkpyte

- Source commit: `9f275bfa7d074321a4747ce3df94e0e59609d189`
- Licence: GPL-3.0 (the same as tmssh)

| Here | From gtkpyte |
|---|---|
| `src/*.{h,cpp}` | `src/` except `main.cpp` and `envcheck.cpp` |
| `pyte/include/`, `pyte/src/` | `pyte/include/`, `pyte/src/` |
| `meson.build` | written for tmssh; builds the same two libraries as gtkpyte's |

`pyte/` is itself vendored in gtkpyte from anytermqt, so the chain is
anytermqt -> gtkpyte -> tmssh. Fixes go upstream first and are copied down;
nothing in this directory is edited in place.

To refresh, from a gtkpyte checkout at the new commit:

```
for f in gtkpyte-terminal keymap mouse palette pty ptysession schemes selection; do cp src/$f.h src/$f.cpp ../tmssh/gtkpyte/src/; done
```

```
rm -rf ../tmssh/gtkpyte/pyte/include ../tmssh/gtkpyte/pyte/src && cp -r pyte/include pyte/src ../tmssh/gtkpyte/pyte/
```

Then update the commit above, and add any new source file to `meson.build`.
