"""One open port, read on its own thread, with the bytes put on a queue.

The reader thread is the reason this is its own module: everything above it in
the UI is single-threaded Tk code that only ever sees a queue, and keeping the
thread here makes that boundary obvious.
"""

import threading

from monitor_ports import open_port, pulse_reset



class SerialSession:
    """One open port and the reader thread pumping its bytes at the GUI.

    Everything the thread produces goes on `out_queue` as `(kind, key, payload)`,
    where kind is data/error/closed. The GUI drains that queue on a timer; no
    widget is ever touched from the reader thread.
    """

    def __init__(self, device, baud, out_queue, key=None):
        self.device = device
        self.baud = baud
        self.out_queue = out_queue
        self.key = key or device
        self.serial = None
        self.thread = None
        self.rx_bytes = 0
        self.tx_bytes = 0
        self._stop = threading.Event()
        self._write_lock = threading.Lock()

    @property
    def is_open(self):
        return self.serial is not None and self.serial.is_open

    def open(self):
        self.serial = open_port(self.device, self.baud)
        self._stop.clear()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self):
        while not self._stop.is_set():
            try:
                waiting = self.serial.in_waiting
                data = self.serial.read(waiting if waiting else 1)
            except Exception as exc:
                if not self._stop.is_set():
                    self.out_queue.put(("error", self.key, "Read failed: {}".format(exc)))
                break
            if data:
                self.rx_bytes += len(data)
                self.out_queue.put(("data", self.key, data))
        self.out_queue.put(("closed", self.key, b""))

    def write(self, data):
        if not self.is_open:
            raise IOError("port is not open")
        with self._write_lock:
            self.serial.write(data)
            self.tx_bytes += len(data)

    def set_baud(self, baud):
        self.baud = baud
        if self.is_open:
            self.serial.baudrate = baud

    def set_control_line(self, name, value):
        """Set DTR or RTS, for boards that need a particular idle state."""
        if self.is_open:
            setattr(self.serial, name, bool(value))

    def reset_board(self):
        if self.is_open:
            pulse_reset(self.serial)

    def send_break(self, duration=0.25):
        if self.is_open:
            self.serial.send_break(duration)

    def close(self):
        self._stop.set()
        if self.thread is not None:
            self.thread.join(timeout=1.0)
        if self.serial is not None:
            try:
                self.serial.close()
            except Exception:
                pass
