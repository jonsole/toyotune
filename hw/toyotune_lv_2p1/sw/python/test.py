import os
import sys
import threading
import serial

ser = serial.Serial('COM6', 115200, timeout=0.1, parity=serial.PARITY_NONE)

while True:
    s = ser.read(100)       # read up to one hundred bytes
    if len(s):
        print(s.decode('utf-8'), end='')


