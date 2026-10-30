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
Zephyr finds `zephyr/module.yml` and processes the module for every sysbuild image, including MCUboot.
The modules build only in images that enable `CONFIG_ATT_MODULES`.
Enable it from the application's `Kconfig`, which the bootloader images do not source:

```kconfig
config ATT_MODULES
	default y
```

The application's `CMakeLists.txt` adds only its own sources, for example `src/main.c`.

## Register your own data source

An application module can store its samples and send them to the cloud from storage batches, without changes to this repository.
Register the data type with `STORAGE_DATA_TYPE_ADD()` from `storage.h`, and the cloud sender with `CLOUD_STORAGE_SENDER_DEFINE()` from `cloud_storage_sender.h`.
Both macros can be used in any application source file:

```c
#include "storage.h"
#include "cloud.h"
#include "cloud_storage_sender.h"

#define STORAGE_TYPE_SOIL 32

static bool soil_check(const struct soil_sensor_msg *msg)
{
	return msg->type == SOIL_SENSOR_SAMPLE_RESPONSE;
}

static void soil_extract(const struct soil_sensor_msg *msg, struct soil_sensor_msg *data)
{
	*data = *msg;
}

STORAGE_DATA_TYPE_ADD(SOIL, STORAGE_TYPE_SOIL, soil_sensor_chan, struct soil_sensor_msg,
		      struct soil_sensor_msg, soil_check, soil_extract);

static int soil_send(const void *data, size_t len)
{
	const struct soil_sensor_msg *soil = data;
	char json[64];

	snprintk(json, sizeof(json), "{\"appId\":\"SOIL\",\"moisture\":%u}", soil->moisture_pct);

	return cloud_json_message_send(json);
}

CLOUD_STORAGE_SENDER_DEFINE(STORAGE_TYPE_SOIL, struct soil_sensor_msg, soil_send);
```

Follow these rules:

* Use a storage type id from 32 to 254, written as a decimal literal or as a macro that expands to one.
  The ids 2 to 31 are reserved for this repository.
  Two registrations with the same id fail to link.
* Set `CONFIG_APP_STORAGE_EXT_MAX_DATA_SIZE` to at least the size of the largest stored data type of the application, and `CONFIG_APP_STORAGE_EXT_MAX_MSG_SIZE` to at least the size of the largest message type.
  A registration that does not fit fails to build and names the option to increase.
* Count the application data types in `CONFIG_APP_STORAGE_MAX_TYPES`.
* A sender runs in the cloud module thread.
  It returns 0 when the item is sent, `-EINVAL` or `-ENOTSUP` to drop the item, and any other negative errno to keep the item for the next batch.
