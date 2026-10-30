# ATT tracker application

This application builds the out-of-the-box Asset Tracker Template (ATT) from [nrfconnect/Asset-Tracker-Template](https://github.com/nrfconnect/Asset-Tracker-Template) commit `8982086`.
`main.c`, `prj.conf`, the board files, the sysbuild configuration, and the overlays are unchanged.
The ATT modules come from the `att-modules` repository, which west fetches into `modules/lib/att-modules`.

The application differs from the template in three files:

* `app/CMakeLists.txt` adds only `src/main.c` and `memfault/`.
* `app/Kconfig` enables `CONFIG_ATT_MODULES` and no longer sources the module Kconfig files.
* `app/src/Kconfig.main` no longer defines the `APP` log level, because `att-modules` defines it.

## Set up the workspace

Run the following commands:

```shell
mkdir tracker-ws && cd tracker-ws
git clone -b cursor/att-tracker-app-2691 https://github.com/jhn-nordic/Asset-Tracker-Template att-tracker
west init -l att-tracker
west update
```

## Build the application

Run the following commands from the `att-tracker` directory:

```shell
west build -p -b thingy91x/nrf9151/ns app -d build_thingy91x
west build -p -b nrf9151dk/nrf9151/ns app -d build_nrf9151dk
```

To use another version of the modules, change the `att-modules` revision in `west.yml` and run `west update att-modules`.
