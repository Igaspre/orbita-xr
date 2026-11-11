#!/usr/bin/env python3
"""Pretends to be the IMU of an XREAL One / 1S.

    python3 fake_glasses.py [host] [port] [yaw_degrees] [turn|calib]

Serves 84 byte frames over TCP like the glasses do. "turn": still for 1.5 s,
then turns left by yaw_degrees in one second. "calib": still 4 s, right 45,
pause, up 25 (what the calibration wizard asks for). The sensor is mounted at
an odd angle on purpose.
"""
import math
import socket
import struct
import sys
import time

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 52998
YAW_DEG = float(sys.argv[3]) if len(sys.argv) > 3 else 35.0
SCRIPT = sys.argv[4] if len(sys.argv) > 4 else "turn"
RATE = 1000


def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw * bw - ax * bx - ay * by - az * bz, aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx, aw * bz + ax * by - ay * bx + az * bw)


def qrot(q, v):
    w, x, y, z = q
    p = (0.0, v[0], v[1], v[2])
    r = qmul(qmul(q, p), (w, -x, -y, -z))
    return r[1:]


def axis_angle(ax, ang):
    n = math.sqrt(sum(c * c for c in ax))
    s = math.sin(ang / 2)
    return (math.cos(ang / 2), ax[0] / n * s, ax[1] / n * s, ax[2] / n * s)


SENSOR_FROM_HEAD = axis_angle((0.3, -1.0, 0.5), math.radians(127))
HEADER = bytes([0x28, 0x36, 0x00, 0x00, 0x00, 0x80])
TAG = bytes([0x00, 0x40, 0x1F, 0x00, 0x00, 0x40])


def frame(gyro_head, up_head, ts_ns):
    g = qrot(SENSOR_FROM_HEAD, gyro_head)
    a = qrot(SENSOR_FROM_HEAD, tuple(c * 9.80665 for c in up_head))
    f = bytearray(84)
    f[0:6] = HEADER
    f[14:22] = struct.pack("<Q", ts_ns)
    f[34:46] = struct.pack("<3f", *g)
    f[46:58] = struct.pack("<3f", *a)
    f[60:66] = TAG
    return bytes(f)


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(1)
    print(f"fake glasses on {HOST}:{PORT}", flush=True)
    while True:
        conn, addr = srv.accept()
        print("client connected", addr, flush=True)
        head = (1.0, 0.0, 0.0, 0.0)
        t0 = time.monotonic()
        n = 0
        ts = 5_000_000_000
        try:
            while True:
                # keep ~RATE samples per second of wall time
                target = int((time.monotonic() - t0) * RATE)
                chunk = bytearray()
                while n < target:
                    t = n / RATE
                    pitch_rate = 0.0
                    if SCRIPT == "calib":
                        yaw_rate = -math.radians(45) if 4.0 <= t < 5.0 else 0.0
                        pitch_rate = math.radians(25) if 6.0 <= t < 7.0 else 0.0
                    else:
                        yaw_rate = math.radians(YAW_DEG) if 1.5 <= t < 2.5 else 0.0
                    up_in_head = qrot((head[0], -head[1], -head[2], -head[3]), (0, 1, 0))
                    omega = tuple(c * yaw_rate for c in up_in_head)
                    omega = (omega[0] + pitch_rate, omega[1], omega[2])
                    dt = 1.0 / RATE
                    ang = math.sqrt(sum(c * c for c in omega)) * dt
                    if ang > 0:
                        head = qmul(head, axis_angle(omega, ang))
                    up_head = qrot((head[0], -head[1], -head[2], -head[3]), (0, 1, 0))
                    ts += 1_000_000
                    chunk += frame(omega, up_head, ts)
                    if n % 997 == 0:
                        chunk += b"\x01\x02\x28\x36"  # junk the reader must skip
                    n += 1
                # odd sized writes, like a real socket
                i = 0
                while i < len(chunk):
                    step = 37 + (i % 211)
                    conn.sendall(chunk[i:i + step])
                    i += step
                time.sleep(0.004)
        except (BrokenPipeError, ConnectionResetError):
            print("client gone", flush=True)
        finally:
            conn.close()


if __name__ == "__main__":
    main()
