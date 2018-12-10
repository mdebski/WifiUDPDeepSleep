# esp8266 + ds18b20 wifi temperature sensor

Power consumption:
 1. sleep: ~0.17 mA (all the time)
 2. measure: ~15 mA (<32 ms every 15 s)
 3. send: ~80mA (4s every 15 min)

Average: 0.17*1 + 15 * (32.0 / 15000) + 80.0 * (4.0/(15*60)) = 0.55 mA

