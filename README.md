# ATT modules

This repository holds the Asset Tracker Template (ATT) modules as a Zephyr module named `att-modules`.
An application pulls it in through west and keeps only `main.c` and its own configuration.

The contents come from [nrfconnect/Asset-Tracker-Template](https://github.com/nrfconnect/Asset-Tracker-Template) commit `8982086`:

* `modules/` holds the button, cloud, environmental, fota, led, location, network, power, storage, and uart_power_control modules.
* `common/` holds `app_common.h`, the watchdog helper, and the fuel gauge battery model.
* `cbor/` holds the device shadow codec.

The `CONFIG_APP_*` Kconfig symbols keep their template names.
Each optional module builds only when its `CONFIG_APP_<MODULE>` symbol is enabled.
The location module is optional, and the other modules build without it.

## Pull the modules into an application

Add the repository to the application's `west.yml`:

```yaml
manifest:
  remotes:
    - name: ncs
      url-base: https://github.com/nrfconnect
    - name: jhn
      url-base: https://github.com/jhn-nordic
  projects:
    - name: nrf
      remote: ncs
      repo-path: sdk-nrf
      revision: v3.5.0-preview3
      import: true
    - name: att-modules
      remote: jhn
      repo-path: Asset-Tracker-Template
      revision: cursor/att-modules-2691
      path: modules/lib/att-modules
```

Run `west update`.
Zephyr finds `zephyr/module.yml`, adds the module sources to the `app` target, and adds the module menu to Kconfig.
The application's `CMakeLists.txt` adds only its own sources, for example `src/main.c`.
