#!/usr/bin/env python
# -*- coding: utf8 -*-

import socket
import struct
from datetime import datetime, timedelta
from influxdb import InfluxDBClient

REPLY_FMT = '4sIB'
PROTOCOL_MAGIC = "tTp1"

class Header(object):
    fmt = '4sIIB'
    len = struct.calcsize(fmt)

    def __init__(self, data):
        assert len(data) >= Header.len
        self.magic, self.id, measurements_timedelta_ms, self.data_count = (
                struct.unpack(Header.fmt, data[:Header.len])
        )
        self.measurements_timedelta = timedelta(milliseconds=measurements_timedelta_ms)

    def make_reply(self):
        return struct.pack(REPLY_FMT, self.magic, self.id, self.data_count)

    def __str__(self):
        return "HEADER %s %x %d %s" % (self.magic, self.id, self.data_count,
                self.measurements_timedelta)

class Measurement(object):
    fmt = 'HHH'
    len = struct.calcsize(fmt)

    def __init__(self, i, data):
        assert len(data) >= Measurement.len
        self.index = i
        temperature_cC, voltage_1_1024_V, self.time_awake_ms = (
                struct.unpack(Measurement.fmt, data[:Measurement.len])
        )
        # ds18B20 error code
        if temperature_cC == 8500:
            self.temperature = None
        else:
            self.temperature = float(temperature_cC) / 100
        self.voltage = round(float(voltage_1_1024_V) / 1024, 3)

    def __str__(self):
        tmp = '%.2f °C' % self.temperature if self.temperature else "None"
        return "MEASUREMENT %d %s, %.2f V, %d ms" % (self.index, tmp,
                    self.voltage, self.time_awake_ms)

def parse_data(data):
    if len(data) < Header.len:
        print "Message too short."
        return False
    hdr = Header(data)

    print hdr

    if hdr.magic != PROTOCOL_MAGIC:
        print "Bad magic"
        return False

    if hdr.len + hdr.data_count * Measurement.len != len(data):
        print "Bad packet length: expected %d + %d, got %d" % (hdr.len,
                hdr.data_count*Measurement.len, len(data))
        return False

    res = []

    for i in xrange(hdr.data_count):
        pos = hdr.len + i * Measurement.len
        measurement = Measurement(i, data[pos:])
        res.append(measurement)
        print measurement

    return hdr, res


def main():
  client = InfluxDBClient('localhost', 8086, '', '', 'sensors')
  sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
  sock.bind(('0.0.0.0', 1234))
  last_ids = {}
  last_recv_time = {}
  while True:
    data, addr = sock.recvfrom(1024)
    recv_time = datetime.now()
    print recv_time, len(data), data.encode('hex')

    parsed = parse_data(data)
    if not parsed:
        continue

    hdr, res = parsed

    reply = hdr.make_reply()
    print "Sending reply", addr, reply.encode('hex')
    sock.sendto(reply, addr)

    if hdr.id in last_ids:
        print "Duplicate."
        continue

    last_ids[hdr.id] = recv_time
    last_ids = {k : v for k,v in last_ids.items() if (datetime.now() - v) < timedelta(minutes=30)}

    points = [{
        "measurement": "temperature_sensor",
        "tags": {
            "host": "python_forwarder",
            "sensor_port": addr[1],
            "sensor_host": addr[0],
        },
        "time": recv_time - ((hdr.data_count - m.index - 1) * hdr.measurements_timedelta),
        "fields": {
            "temperature": m.temperature,
            "time_awake_ms": m.time_awake_ms,
            "voltage": m.voltage,
        },
        } for m in res]
    if last_recv_time.get(addr[1], None):
        points.append({
            "measurement": "temperature_sensor_recv",
            "tags": {
                "host": "python_forwarder",
                "sensor_port": addr[1],
                "sensor_host": addr[0],
            },
            "time": recv_time,
            "fields": {
                "time_since_last_s": (recv_time - last_recv_time[addr[1]]).total_seconds(),
            },
        })
    client.write_points(points)
    last_recv_time[addr[1]] = recv_time
    print "Sent to DB."

if __name__ == "__main__":
    main()
