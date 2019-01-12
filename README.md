# esp8266 (wemos d1 mini board) + ds18b20 wifi temperature sensor

BoM

* Wemos D1 mini
* DS18B20
* 4k7
* switch
* 2xAA battery pack

See schematic and photo.

# power consumption

 1. sleep: ~0.17 mA (all the time)
 2. measure: ~15 mA (<32 ms every 15 s)
 3. send: ~80mA (4s every 15 min)

Average: 0.17 * 1 + 15 * (32.0 / 15000) + 80.0 * (4.0/(15 * 60)) = 0.55 mA

