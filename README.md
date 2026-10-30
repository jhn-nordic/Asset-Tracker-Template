# ATT sensor application

This application is a stationary soil sensor built on the Asset Tracker Template (ATT) modules.
It starts from the [tracker application](https://github.com/jhn-nordic/Asset-Tracker-Template/tree/cursor/att-tracker-app-2691) and pulls the same `att-modules` repository through west.

## Differences from the tracker application

* `prj.conf` sets `CONFIG_APP_LOCATION=n`, so no location module code is compiled.
* `prj.conf` and the board configurations drop the location library, GNSS, A-GNSS, Wi-Fi location, and location metrics options.
* `main.c` does not wait for the location module or trigger location searches.
* `app/src/modules/soil_sensor/` holds a mock soil sensor module that lives in this repository, not in `att-modules`.
  It publishes moisture and temperature on `soil_sensor_chan`, as a bounded random walk.
  `main.c` uses `soil_sensor_chan` directly, so keep `CONFIG_APP_SOIL_SENSOR` enabled.

Each sample period, `main.c` requests a soil sample together with the battery and environmental samples.
The soil sample response ends the sampling state, which the location search did in the tracker.
When the cloud is connected, `main.c` sends the sample to nRF Cloud as a `CLOUD_PAYLOAD_JSON` message:

```json
{"appId":"SOIL","moisture":42,"temp":15.3}
```

When the cloud is disconnected, `main.c` drops the sample.

On the Thingy:91 X, the nRF70 Wi-Fi driver is still enabled, because the board overlay and `Kconfig.sysbuild` enable it.
Nothing in this application uses Wi-Fi.

## Known gap: the storage and cloud registry

The storage module buffers samples through `DATA_SOURCE_LIST` in `att-modules/modules/storage/storage_data_types.h`.
The cloud module encodes each buffered type in `att-modules/modules/cloud/cloud.c`.
Both lists are central, so an application module cannot add a data type without editing `att-modules`.
For this reason, soil samples bypass storage and go straight to the cloud module, and the application cannot buffer them while disconnected.
A registry that applications can extend would remove this gap.

## Set up the workspace

Run the following commands:

```shell
mkdir sensor-ws && cd sensor-ws
git clone -b cursor/att-sensor-app-2691 https://github.com/jhn-nordic/Asset-Tracker-Template att-sensor
west init -l att-sensor
west update
```

## Build the application

Run the following commands from the `att-sensor` directory:

```shell
west build -p -b thingy91x/nrf9151/ns app -d build_thingy91x
west build -p -b nrf9151dk/nrf9151/ns app -d build_nrf9151dk
```
