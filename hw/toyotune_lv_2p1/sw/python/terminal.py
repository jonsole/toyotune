import serial

with serial.Serial('COM3', 115200, timeout=0) as ser:
    while True:
        x = ser.read(80)
        print(x.decode(encoding="ascii", errors="replace"), end='')

