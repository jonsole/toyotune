import asyncio
import serial
import serial_asyncio

async def main(loop):
    reader, writer = await serial_asyncio.open_serial_connection(url='COM5', baudrate=115200)
    messages = [b'foo\n', b'bar\n', b'baz\n', b'qux\n']
    sent = send(writer, messages)
    received = recv(reader)
    await asyncio.wait([sent, received])


async def send(w, msgs):
    for msg in msgs:
        w.write(msg)
        print(f'sent: {msg.decode().rstrip()}')
        await asyncio.sleep(0.5)
    w.write(b'DONE\n')
    print('Done sending')

async def recv(r):
    while True:
        msg = await r.readuntil(b'\n')
        if msg.rstrip() == b'DONE':
            print('Done receiving')
            break
        print(f'received: {msg.rstrip().decode()}')

loop = asyncio.get_event_loop()
loop.run_until_complete(main(loop))
loop.close()


# ESP Packet Header
# 
# Byte  Bit  Description
# 0     7:6  Channel - 0 Link Control
#				       1,2,3 - Application specific
#	    5:3  Tx Sequence Number - 0:7  
#	    2:0  Next Expected Seq. Num. - 0:7 
#
# 1     7    CRC Present 0 = No CRC
#				       1 = CRC at end of packet
#       6:0  Payload length 0:127








