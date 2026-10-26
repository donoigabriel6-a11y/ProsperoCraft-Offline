# Title boilerplate

This folder is the title boilerplate that `tools/build/make_jvm_title.sh` turns into the ProsperoCraft title (`~/ps5probe` on the build machine).
It comes from **ps5-native-app-boilerplate** by BlackBearReloaded (GPL-3.0-or-later, commit `dd44bbd` "Credit SvenGDK's SharpProspero in the native tooling"), the same author as the Kit launcher in `launcher/`. The upstream repository was deleted by its author, so this copy is kept here. Its license and notices stay as they were (`LICENSE`, `THIRD_PARTY_NOTICES.md`).

Changes on top of upstream: `sce_sys/param.json` carries the ProsperoCraft identity (title id `PPSA27272`, content id, name) and `tools/build.sh` carries the whole-archive change `make_jvm_title.sh` applies for `vendor/whole-*.a`.

Not stored here (generated or your own artwork): `vendor/whole-*.a` and `.jdkobj*` (built from the JDK sources by `make_jvm_title.sh`), `src/kit` (replaced by `launcher/`), `src/jvm_host.cpp` and `src/ps5_symtab.c` (generated / replaced by `src/host/jvm_host.cpp`), `include/ps5_conf.h`, and the icon / picture / sound files in `sce_sys/` (put yours in the folder `ASSETS` points at, see `docs/BUILDING.md`).
