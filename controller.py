import json
import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext, ttk
import serial
import serial.tools.list_ports


class ArduinoControllerApp:

  def __init__(self, root):
    self.root = root
    self.root.title("Arduino Home Automation Controller")
    self.root.geometry("620x560")
    self.root.resizable(False, False)

    self.ser = None
    self.is_connected = False
    self.read_thread = None

    # Track relay states (Index 0-3 for Relays 1-4)
    self.relay_states = [False, False, False, False]
    self.relay_buttons = []
    self.last_sent_dimmer = 0

    self.create_widgets()
    self.refresh_ports()

  def create_widgets(self):
    # --- CONNECTION FRAME ---
    conn_frame = ttk.LabelFrame(
        self.root, text="Serial Connection", padding=10
    )
    conn_frame.pack(fill="x", padx=10, pady=10)

    ttk.Label(conn_frame, text="Port:").grid(
        row=0, column=0, sticky="w", padx=5
    )
    self.port_combo = ttk.Combobox(conn_frame, width=15, state="readonly")
    self.port_combo.grid(row=0, column=1, padx=5)

    refresh_btn = ttk.Button(
        conn_frame, text="Refresh", command=self.refresh_ports
    )
    refresh_btn.grid(row=0, column=2, padx=5)

    ttk.Label(conn_frame, text="Baudrate:").grid(
        row=0, column=3, sticky="w", padx=5
    )
    self.baud_entry = ttk.Entry(conn_frame, width=10)
    self.baud_entry.insert(0, "115200")
    self.baud_entry.grid(row=0, column=4, padx=5)

    self.connect_btn = ttk.Button(
        conn_frame, text="Connect", command=self.toggle_connection
    )
    self.connect_btn.grid(row=0, column=5, padx=10)

    # --- RELAYS FRAME (TOGGLE BUTTONS) ---
    relay_frame = ttk.LabelFrame(self.root, text="Relay Controls", padding=10)
    relay_frame.pack(fill="x", padx=10, pady=5)

    for i in range(1, 5):
      btn = tk.Button(
          relay_frame,
          text=f"Relay {i}\nOFF",
          font=("Arial", 10, "bold"),
          width=12,
          height=3,
          bg="#e0e0e0",
          fg="#000000",
          relief="raised",
          command=lambda r=i: self.toggle_relay(r),
      )
      btn.pack(side="left", expand=True, padx=5, pady=5)
      self.relay_buttons.append(btn)

    # --- DIMMER FRAME ---
    dimmer_frame = ttk.LabelFrame(
        self.root, text="Dimmer Control (0-9)", padding=10
    )
    dimmer_frame.pack(fill="x", padx=10, pady=5)

    d_sub = ttk.Frame(dimmer_frame)
    d_sub.pack(fill="x", padx=5)

    ttk.Button(
        d_sub, text="MIN (0)", command=lambda: self.send_command("DIMMER 0")
    ).pack(side="left", padx=5)
    ttk.Button(
        d_sub, text="Step -", command=lambda: self.send_command("DIMM_DN")
    ).pack(side="left", padx=5)

    # Slider without continuous command firing
    self.dimmer_slider = ttk.Scale(d_sub, from_=0, to=9, orient="horizontal")
    self.dimmer_slider.pack(
        side="left", expand=True, fill="x", padx=10, pady=5
    )
    # Bind release event so it only sends command when user drops the slider handle
    self.dimmer_slider.bind("<ButtonRelease-1>", self.on_slider_release)

    ttk.Button(
        d_sub, text="Step +", command=lambda: self.send_command("DIMM_UP")
    ).pack(side="left", padx=5)
    ttk.Button(
        d_sub, text="MAX (9)", command=lambda: self.send_command("DIMMER 9")
    ).pack(side="left", padx=5)

    # --- GLOBAL & UTILS FRAME ---
    global_frame = ttk.LabelFrame(
        self.root, text="Global Commands & Status", padding=10
    )
    global_frame.pack(fill="x", padx=10, pady=5)

    g_sub = ttk.Frame(global_frame)
    g_sub.pack(fill="x")

    ttk.Button(
        g_sub, text="ALL ON", command=lambda: self.send_command("ALL_ON")
    ).pack(side="left", expand=True, padx=5)
    ttk.Button(
        g_sub, text="ALL OFF", command=lambda: self.send_command("ALL_OFF")
    ).pack(side="left", expand=True, padx=5)
    ttk.Button(
        g_sub, text="Get Status", command=lambda: self.send_command("STATUS")
    ).pack(side="left", expand=True, padx=5)

    # --- LOG / CONSOLE OUTPUT ---
    log_frame = ttk.LabelFrame(
        self.root, text="Serial Console / Feedback", padding=10
    )
    log_frame.pack(fill="both", expand=True, padx=10, pady=5)

    self.console_log = scrolledtext.ScrolledText(
        log_frame, height=7, state="disabled", font=("Courier", 9)
    )
    self.console_log.pack(fill="both", expand=True)

  def refresh_ports(self):
    ports = [port.device for port in serial.tools.list_ports.comports()]
    self.port_combo["values"] = ports
    if ports:
      self.port_combo.current(0)

  def toggle_connection(self):
    if not self.is_connected:
      selected_port = self.port_combo.get()
      baudrate_str = self.baud_entry.get()

      if not selected_port:
        messagebox.showerror("Error", "Please select a valid COM port.")
        return

      try:
        baudrate = int(baudrate_str)
        self.ser = serial.Serial(selected_port, baudrate, timeout=1)
        self.is_connected = True
        self.connect_btn.config(text="Disconnect")
        self.port_combo.config(state="disabled")
        self.baud_entry.config(state="disabled")

        # Start background thread to listen for incoming Arduino serial data
        self.read_thread = threading.Thread(target=self.read_serial, daemon=True)
        self.read_thread.start()

        self.log_message(f"Connected to {selected_port} at {baudrate} baud.\n")
        self.send_command("STATUS")
      except Exception as e:
        messagebox.showerror("Connection Error", str(e))
    else:
      try:
        if self.ser and self.ser.is_open:
          self.ser.close()
      except Exception as e:
        print(e)

      self.is_connected = False
      self.connect_btn.config(text="Connect")
      self.port_combo.config(state="readonly")
      self.baud_entry.config(state="normal")
      self.log_message("Disconnected.\n")

  def toggle_relay(self, relay_num):
    idx = relay_num - 1
    current_state = self.relay_states[idx]
    target_state = not current_state
    cmd = f"POWER{relay_num} {'ON' if target_state else 'OFF'}"
    self.send_command(cmd)

  def send_command(self, cmd):
    if not self.is_connected or not self.ser or not self.ser.is_open:
      messagebox.showwarning(
          "Warning", "Not connected to any serial device!"
      )
      return

    try:
      full_cmd = cmd + "\n"
      self.ser.write(full_cmd.encode("utf-8"))
      self.log_message(f">> {cmd}\n")
    except Exception as e:
      messagebox.showerror("Serial Write Error", str(e))

  def on_slider_release(self, event):
    val_int = int(round(self.dimmer_slider.get()))
    # Snap slider position directly to the nearest discrete integer step
    self.dimmer_slider.set(val_int)
    if val_int != self.last_sent_dimmer:
      self.last_sent_dimmer = val_int
      self.send_command(f"DIMMER {val_int}")

  def read_serial(self):
    while self.is_connected:
      try:
        if self.ser and self.ser.in_waiting > 0:
          line = self.ser.readline().decode("utf-8", errors="ignore").strip()
          if line:
            self.log_message(f"<< {line}\n")
            self.parse_arduino_feedback(line)
      except Exception:
        break

  def parse_arduino_feedback(self, line):
    try:
      data = json.loads(line)
      for i in range(1, 5):
        key = f"POWER{i}"
        if key in data:
          is_on = data[key] == "ON"
          self.relay_states[i - 1] = is_on
          self.root.after(
              0, lambda idx=i - 1, val=is_on: self.update_ui_relay(idx, val)
          )

      if "Dimmer" in data:
        dim_val = data["Dimmer"]
        self.last_sent_dimmer = dim_val
        self.root.after(0, lambda v=dim_val: self.dimmer_slider.set(v))
    except json.JSONDecodeError:
      pass

  def update_ui_relay(self, idx, is_on):
    btn = self.relay_buttons[idx]
    if is_on:
      btn.config(text=f"Relay {idx+1}\nON", bg="#4CAF50", fg="#FFFFFF")
    else:
      btn.config(text=f"Relay {idx+1}\nOFF", bg="#e0e0e0", fg="#000000")

  def log_message(self, msg):
    self.console_log.config(state="normal")
    self.console_log.insert(tk.END, msg)
    self.console_log.see(tk.END)
    self.console_log.config(state="disabled")


if __name__ == "__main__":
  root = tk.Tk()
  app = ArduinoControllerApp(root)
  root.mainloop()
