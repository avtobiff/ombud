# Simple TCP socket server, basically copied from Python docs

import socket
import threading
import SocketServer

class ThreadedTCPRequestHandler(SocketServer.BaseRequestHandler):
    def handle(self):
        cur_thread = threading.current_thread()
        self.request.sendall("0123456789abcdefghijklmnopqrst\r\n") # 32 B

class ThreadedTCPServer(SocketServer.ThreadingMixIn, SocketServer.TCPServer):
    pass

if __name__ == "__main__":
    HOST, PORT = "localhost", 9999

    server = ThreadedTCPServer((HOST, PORT), ThreadedTCPRequestHandler)
    ip, port = server.server_address
    server.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, True)

    server_thread = threading.Thread(target=server.serve_forever)
    server_thread.daemon = True
    server_thread.start()
    print "Server loop running in thread:", server_thread.name

    server.serve_forever()
