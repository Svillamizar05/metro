import socket, threading, queue, argparse
import tkinter as tk
from tkinter import ttk

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=5000)
    p.add_argument("--admin-token", default=None)  # opcional, por si luego activas roles
    return p.parse_args()

class MetroClientGUI:
    def __init__(self, host, port, admin_token=None):
        self.host, self.port, self.admin_token = host, port, admin_token
        self.sock = None
        self.q = queue.Queue()

        self.root = tk.Tk()
        self.root.title("Metro Telemetría (Python)")
        self._build_ui()
        self._connect()
        self.root.after(150, self._ui_tick)

    def _build_ui(self):
        frm = ttk.Frame(self.root, padding=12); frm.grid(sticky="nsew")
        self.root.columnconfigure(0, weight=1); self.root.rowconfigure(0, weight=1)

        self.speed = tk.StringVar(value="--")
        self.batt  = tk.StringVar(value="--")
        self.sta   = tk.StringVar(value="--")
        self.dir   = tk.StringVar(value="--")
        self.status= tk.StringVar(value="Desconectado")

        r=0
        ttk.Label(frm, text="Servidor:").grid(column=0,row=r,sticky="w")
        ttk.Label(frm, text=f"{self.host}:{self.port}", foreground="gray").grid(column=1,row=r,sticky="w"); r+=1
        ttk.Label(frm, text="Velocidad (km/h):").grid(column=0,row=r,sticky="w")
        ttk.Label(frm, textvariable=self.speed, font=("TkDefaultFont",12,"bold")).grid(column=1,row=r,sticky="w"); r+=1
        ttk.Label(frm, text="Batería (%):").grid(column=0,row=r,sticky="w")
        ttk.Label(frm, textvariable=self.batt, font=("TkDefaultFont",12,"bold")).grid(column=1,row=r,sticky="w"); r+=1
        ttk.Label(frm, text="Estación:").grid(column=0,row=r,sticky="w")
        ttk.Label(frm, textvariable=self.sta, font=("TkDefaultFont",12,"bold")).grid(column=1,row=r,sticky="w"); r+=1
        ttk.Label(frm, text="Dirección:").grid(column=0,row=r,sticky="w")
        ttk.Label(frm, textvariable=self.dir, font=("TkDefaultFont",12,"bold")).grid(column=1,row=r,sticky="w"); r+=1

        ttk.Separator(frm, orient="horizontal").grid(column=0,row=r,columnspan=2,sticky="ew",pady=8); r+=1
        btns = ttk.Frame(frm); btns.grid(column=0,row=r,columnspan=2,pady=2)
        ttk.Button(btns, text="SPEED_UP",  command=lambda: self._send("CMD SPEED_UP")).grid(column=0,row=0,padx=4)
        ttk.Button(btns, text="SLOW_DOWN", command=lambda: self._send("CMD SLOW_DOWN")).grid(column=1,row=0,padx=4)
        ttk.Button(btns, text="STOPNOW",   command=lambda: self._send("CMD STOPNOW")).grid(column=2,row=0,padx=4)
        ttk.Button(btns, text="STARTNOW",  command=lambda: self._send("CMD STARTNOW")).grid(column=3,row=0,padx=4); r+=1
        ttk.Label(frm, textvariable=self.status, foreground="gray").grid(column=0,row=r,columnspan=2,sticky="w",pady=6)

    def _connect(self):
        try:
            self.sock = socket.create_connection((self.host, self.port), timeout=5)
            self.sock.settimeout(2.0)
            self.status.set("Conectado")
            if self.admin_token:
                self._send(f"ADMIN token={self.admin_token}")
            threading.Thread(target=self._recv, daemon=True).start()
        except Exception as e:
            self.status.set(f"Fallo conexión: {e}")
            self.root.after(2000, self._connect)

    def _send(self, line):
        try:
            if self.sock: self.sock.sendall((line+"\n").encode("utf-8"))
            self.status.set("Enviado: " + line)
        except Exception as e:
            self.status.set(f"Error envío: {e}")

    def _recv(self):
        buf = b""
        try:
            while True:
                try:
                    chunk = self.sock.recv(4096)
                except socket.timeout:
                    continue
                if not chunk: break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n",1)
                    s = line.decode("utf-8","ignore").strip()
                    if s: self.q.put(s)
        except Exception as e:
            self.q.put("ERROR "+str(e))
        finally:
            self.q.put("CLOSED")

    def _ui_tick(self):
        import queue
        try:
            while True:
                s = self.q.get_nowait()
                if s == "CLOSED":
                    self.status.set("Conexión cerrada. Reintentando...")
                    try: self.sock.close()
                    except: pass
                    self.root.after(1500, self._connect)
                elif s.startswith("ERROR "):
                    self.status.set(s)
                elif s == "ACK":
                    self.status.set("ACK")
                elif s.startswith("NACK"):
                    self.status.set(s)
                else:
                    parts = s.split()
                    kind = parts[0]
                    kv = dict(p.split("=",1) for p in parts[1:] if "=" in p)
                    if kind == "TELEMETRY":
                        self.speed.set(kv.get("speed","--"))
                        self.batt.set(kv.get("battery","--"))
                        self.sta.set(kv.get("station","--"))
                        self.dir.set(kv.get("direction","--"))
                        self.status.set("Telemetría recibida")
                    elif kind == "EVENT":
                        self.status.set("Evento: " + s)
                    else:
                        self.status.set("Otro: " + s)
        except queue.Empty:
            pass
        self.root.after(150, self._ui_tick)

    def run(self): self.root.mainloop()

if __name__ == "__main__":
    a = parse_args()
    MetroClientGUI(a.host, a.port, a.admin_token).run()

