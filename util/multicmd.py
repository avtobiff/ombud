import socket
import sys


def send_cmds(sock, commands):
    sock.send(commands)

    try:
        while True:
            data = sock.recv(8192)
            if not data: break
            sys.stdout.write(data)
    except socket.timeout:
        print "timeout"


sock = socket.socket()
sock.settimeout(1)
sock.connect(("localhost", 8090))

cmds = ["localhost:22", "127.0.0.1:22", "localhost:9999", "127.0.0.1:9999"]
send_cmds(sock, "\r\n".join(cmds))
send_cmds(sock, "\n".join(cmds))

sock.close()
