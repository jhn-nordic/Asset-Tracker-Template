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

On the Thingy:91 X, the nRF70 Wi-Fi driver is still enabled, because the board overlay and `Kconfig.sysbuild` enable it.
Nothing in this application uses Wi-Fi.

## Soil samples in storage

The soil sensor module registers its samples with the storage and cloud registries of `att-modules`, from `soil_sensor.c` in this repository:

* `STORAGE_DATA_TYPE_ADD()` registers `struct soil_sensor_msg` as storage type 32 on `soil_sensor_chan`.
  The storage module then stores each `SOIL_SENSOR_SAMPLE_RESPONSE`, also while the cloud is disconnected.
* `CLOUD_STORAGE_SENDER_DEFINE()` registers the function that sends a stored sample to nRF Cloud.
  The cloud module calls it for each soil sample in a storage batch.
* `prj.conf` sets `CONFIG_APP_STORAGE_EXT_MAX_DATA_SIZE` to the size of `struct soil_sensor_msg`.
  On the nRF9151 DK, no built-in storage type is enabled, so the storage buffers would otherwise have no room for the sample.

Soil samples follow the storage threshold and batch sending, like the battery and environmental samples.
The sender sends each sample as a JSON message, with the sample time in unix time milliseconds in `ts`:

```json
{"appId":"SOIL","moisture":42,"temp":15.3,"ts":1767225600000}
```

`main.c` does not send soil samples itself.

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
