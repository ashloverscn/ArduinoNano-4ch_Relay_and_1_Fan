import json
import os
import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext, ttk
import serial
import serial.tools.list_ports
import paho.mqtt.client as mqtt

CONFIG_FILE = "arduino_config.json"


class ConfigWindow(tk.Toplevel):
    """Dedicated Configuration Window for Serial & MQTT Settings"""

    def __init__(self, parent, current_config, on_save_callback):
        super().__init__(parent)
        self.title("Connection & MQTT Configuration")
        self.geometry("450x480")
        self.resizable(False, False)
        self.config_data = current_config
        self.on_save_callback = on_save_callback

        self.transient(parent)
        self.grab_set()

        self.create_widgets()
        self.load_values()

    def create_widgets(self):
        main_frame = ttk.Frame(self, padding=15)
        main_frame.pack(fill="both", expand=True)

        ttk.Label(
            main_frame,
            text="Serial & Broker Configuration",
            font=("Arial", 12, "bold"),
        ).pack(anchor="w", pady=(0, 10))

        form_frame = ttk.LabelFrame(
            main_frame, text="Connection Parameters", padding=10
        )
        form_frame.pack(fill="both", expand=True, pady=5)

        self.entries = {}
        fields = [
            ("baud", "Default Baudrate:"),
            ("mqtt_server", "MQTT Server:"),
            ("mqtt_port", "MQTT Port:"),
            ("mqtt_user", "MQTT Username:"),
            ("mqtt_pass", "MQTT Password:", True),
            ("mqtt_topic_cmd", "Command Topic:"),
            ("mqtt_topic_state", "State Topic:"),
        ]

        for item in fields:
            key, label_text = item[0], item[1]
            is_pass = len(item) > 2 and item[2]

            row_frame = ttk.Frame(form_frame)
            row_frame.pack(fill="x", pady=5)

            ttk.Label(row_frame, text=label_text, width=16, anchor="w").pack(
                side="left"
            )
            entry = ttk.Entry(
                row_frame, show="*" if is_pass else None, width=28
            )
            entry.pack(side="right", expand=True, fill="x")
            self.entries[key] = entry

        save_btn = tk.Button(
            main_frame,
            text="Save Configuration",
            bg="#28a745",
            fg="white",
            font=("Arial", 10, "bold"),
            relief="raised",
            command=self.save_and_close,
        )
        save_btn.pack(fill="x", pady=15)

    def load_values(self):
        for key, entry in self.entries.items():
            val = self.config_data.get(key, "")
            entry.delete(0, tk.END)
            entry.insert(0, str(val))

    def save_and_close(self):
        new_config = {}
        for key, entry in self.entries.items():
            val = entry.get()
            if key in ["baud", "mqtt_port"]:
                try:
                    val = int(val)
                except ValueError:
                    val = 115200 if key == "baud" else 8883
            new_config[key] = val

        self.on_save_callback(new_config)
        messagebox.showinfo(
            "Success", "Configuration saved successfully!", parent=self
        )
        self.destroy()


class ArduinoControllerApp:

    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 Home Automation Controller (Stable)")
        self.root.geometry("620x620")
        self.root.resizable(False, False)

        self.ser = None
        self.is_connected = False
        self.read_thread = None

        self.relay_states = [False, False, False, False]
        self.relay_buttons = []
        self.last_sent_dimmer = 0

        self.config = self.load_config()
        self.create_widgets()
        self.refresh_ports()
        self.load_saved_settings()

        # Initialize MQTT in a background thread to prevent startup hanging
        threading.Thread(target=self.init_mqtt_client, daemon=True).start()

        # Handle clean, instant window close event
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

    def load_config(self):
        default_config = {
            "port": "",
            "baud": 115200,
            "mqtt_server": "i11b0026.ala.eu-central-1.emqxsl.com",
            "mqtt_port": 8883,
            "mqtt_user": "admin",
            "mqtt_pass": "admin1234S",
            "mqtt_topic_cmd": "home/esp8266/command",
            "mqtt_topic_state": "home/esp8266/state",
        }
        if os.path.exists(CONFIG_FILE):
            try:
                with open(CONFIG_FILE, "r") as f:
                    loaded = json.load(f)
                    default_config.update(loaded)
            except Exception:
                pass
        return default_config

    def save_config_to_disk(self):
        try:
            with open(CONFIG_FILE, "w") as f:
                json.dump(self.config, f, indent=4)
        except Exception as e:
            print(f"Config Save Error: {e}")

    def update_config_from_dialog(self, new_config):
        def background_update():
            self.config.update(new_config)
            self.config["port"] = self.port_combo.get()
            try:
                self.config["baud"] = int(self.baud_entry.get())
            except ValueError:
                pass
            self.save_config_to_disk()

            if hasattr(self, "mqtt_client"):
                try:
                    self.mqtt_client.loop_stop()
                    self.mqtt_client.disconnect()
                except Exception:
                    pass
            self.init_mqtt_client()
            self.safe_log(">> Configuration updated and MQTT reconnected.\n")

        threading.Thread(target=background_update, daemon=True).start()

    def init_mqtt_client(self):
        broker = self.config.get("mqtt_server")
        port = self.config.get("mqtt_port")
        user = self.config.get("mqtt_user")
        passwd = self.config.get("mqtt_pass")

        self.mqtt_client = mqtt.Client(
            client_id="tkinter_controller_stable",
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            protocol=mqtt.MQTTv5,
        )
        self.mqtt_client.username_pw_set(username=user, password=passwd)
        self.mqtt_client.tls_set()
        self.mqtt_client.on_connect = self.on_mqtt_connect
        self.mqtt_client.on_message = self.on_mqtt_message

        try:
            self.mqtt_client.connect_async(broker, int(port), keepalive=60)
            self.mqtt_client.loop_start()
        except Exception as e:
            self.safe_log(f"MQTT Connection Error: {e}\n")

    def open_config_window(self):
        ConfigWindow(
            self.root, self.config, self.update_config_from_dialog
        )

    def create_widgets(self):
        top_bar = ttk.Frame(self.root)
        top_bar.pack(fill="x", padx=10, pady=(10, 0))

        title_lbl = ttk.Label(
            top_bar, text="ESP32 Home Automation", font=("Arial", 12, "bold")
        )
        title_lbl.pack(side="left", padx=5)

        config_btn = tk.Button(
            top_bar,
            text="⚙️ Config",
            bg="#6c757d",
            fg="white",
            font=("Arial", 9, "bold"),
            relief="flat",
            command=self.open_config_window,
        )
        config_btn.pack(side="right", padx=5)

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

        # --- RELAYS FRAME ---
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

        self.dimmer_slider = ttk.Scale(d_sub, from_=0, to=9, orient="horizontal")
        self.dimmer_slider.pack(
            side="left", expand=True, fill="x", padx=10, pady=5
        )
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
            log_frame, height=6, state="disabled", font=("Courier", 9)
        )
        self.console_log.pack(fill="both", expand=True)

    # --- MQTT Callbacks ---
    def on_mqtt_connect(
        self, client, userdata, flags, reason_code, properties=None
    ):
        if reason_code == 0:
            self.safe_log("Connected to EMQX MQTT Broker.\n")
            t_cmd = self.config.get("mqtt_topic_cmd", "home/esp8266/command")
            t_state = self.config.get("mqtt_topic_state", "home/esp8266/state")
            client.subscribe(t_cmd)
            client.subscribe(t_state)
        else:
            self.safe_log(f"MQTT Connection failed with code {reason_code}\n")

    def on_mqtt_message(self, client, userdata, msg):
        try:
            payload_str = msg.payload.decode("utf-8", errors="ignore")
            self.parse_arduino_feedback(payload_str)
        except Exception:
            pass

    def refresh_ports(self):
        try:
            ports = [port.device for port in serial.tools.list_ports.comports()]
            self.port_combo["values"] = ports
            if ports and not self.port_combo.get():
                self.port_combo.current(0)
        except Exception:
            pass

    def load_saved_settings(self):
        saved_port = self.config.get("port")
        saved_baud = self.config.get("baud")
        available_ports = list(self.port_combo["values"])

        if saved_baud:
            self.baud_entry.delete(0, tk.END)
            self.baud_entry.insert(0, str(saved_baud))

        if saved_port and saved_port in available_ports:
            self.port_combo.set(saved_port)

    def toggle_connection(self):
        if not self.is_connected:
            selected_port = self.port_combo.get()
            baudrate_str = self.baud_entry.get()

            if not selected_port:
                messagebox.showerror("Error", "Please select a valid COM port.")
                return

            try:
                baudrate = int(baudrate_str)
                self.ser = serial.Serial(
                    selected_port, baudrate, timeout=0.5, write_timeout=0.5
                )
                self.is_connected = True
                self.connect_btn.config(text="Disconnect")
                self.port_combo.config(state="disabled")
                self.baud_entry.config(state="disabled")

                self.config["port"] = selected_port
                self.config["baud"] = baudrate
                self.save_config_to_disk()

                self.read_thread = threading.Thread(
                    target=self.read_serial, daemon=True
                )
                self.read_thread.start()

                self.log_message(
                    f"Connected to {selected_port} at {baudrate} baud.\n"
                )
                self.send_command("STATUS")
            except Exception as e:
                messagebox.showerror("Connection Error", str(e))
                self.is_connected = False
        else:
            self.disconnect_serial()

    def disconnect_serial(self):
        self.is_connected = False
        try:
            if self.ser and self.ser.is_open:
                self.ser.cancel_read()
                self.ser.cancel_write()
                self.ser.close()
        except Exception as e:
            print(e)

        try:
            self.connect_btn.config(text="Connect")
            self.port_combo.config(state="readonly")
            self.baud_entry.config(state="normal")
            self.log_message("Disconnected.\n")
        except Exception:
            pass

    def toggle_relay(self, relay_num):
        idx = relay_num - 1
        current_state = self.relay_states[idx]
        target_state = not current_state
        cmd = f"POWER{relay_num} {'ON' if target_state else 'OFF'}"
        self.send_command(cmd)

    def send_command(self, cmd):
        if self.is_connected and self.ser and self.ser.is_open:
            try:
                full_cmd = (cmd + "\n").encode("utf-8")
                self.ser.write(full_cmd)
                self.log_message(f">> {cmd} (Serial)\n")
            except Exception as e:
                self.log_message(f">> Serial Write Error: {e}\n")
                self.disconnect_serial()
        else:
            try:
                mqtt_payload = json.dumps({"command": cmd})
                t_cmd = self.config.get(
                    "mqtt_topic_cmd", "home/esp8266/command"
                )
                self.mqtt_client.publish(t_cmd, mqtt_payload)
                self.log_message(f">> {cmd} (MQTT Broadcast)\n")
            except Exception as e:
                self.log_message(f">> {cmd} (Failed - No Connection)\n")

    def on_slider_release(self, event):
        val_int = int(round(self.dimmer_slider.get()))
        self.dimmer_slider.set(val_int)
        if val_int != self.last_sent_dimmer:
            self.last_sent_dimmer = val_int
            self.send_command(f"DIMMER {val_int}")

    def read_serial(self):
        while self.is_connected:
            try:
                if self.ser and self.ser.is_open:
                    line = self.ser.readline().decode("utf-8", errors="ignore").strip()
                    if line:
                        self.safe_log(f"<< {line}\n")
                        self.parse_arduino_feedback(line)
            except Exception:
                if self.is_connected:
                    self.root.after(0, self.disconnect_serial)
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
                        0,
                        lambda idx=i - 1, val=is_on: self.update_ui_relay(
                            idx, val
                        ),
                    )

            if "Dimmer" in data:
                dim_val = data["Dimmer"]
                self.last_sent_dimmer = dim_val
                self.root.after(0, lambda v=dim_val: self.dimmer_slider.set(v))

            if "type" in data and data["type"] == "state_update":
                if "relays" in data:
                    for i, state in enumerate(data["relays"]):
                        self.relay_states[i] = state
                        self.root.after(
                            0,
                            lambda idx=i, val=state: self.update_ui_relay(
                                idx, val
                            ),
                        )
                if "dimmer" in data:
                    dim_val = data["dimmer"]
                    self.last_sent_dimmer = dim_val
                    self.root.after(
                        0, lambda v=dim_val: self.dimmer_slider.set(v)
                    )
        except json.JSONDecodeError:
            pass

    def update_ui_relay(self, idx, is_on):
        try:
            btn = self.relay_buttons[idx]
            if is_on:
                btn.config(text=f"Relay {idx+1}\nON", bg="#4CAF50", fg="#FFFFFF")
            else:
                btn.config(text=f"Relay {idx+1}\nOFF", bg="#e0e0e0", fg="#000000")
        except Exception:
            pass

    def log_message(self, msg):
        try:
            self.console_log.config(state="normal")
            self.console_log.insert(tk.END, msg)
            self.console_log.see(tk.END)
            self.console_log.config(state="disabled")
        except Exception:
            pass

    def safe_log(self, msg):
        self.root.after(0, lambda: self.log_message(msg))

    def on_closing(self):
        self.is_connected = False
        try:
            if self.ser and self.ser.is_open:
                self.ser.cancel_read()
                self.ser.cancel_write()
                self.ser.close()
        except Exception:
            pass

        def cleanup_mqtt():
            try:
                if hasattr(self, "mqtt_client"):
                    self.mqtt_client.loop_stop(force=True)
                    self.mqtt_client.disconnect()
            except Exception:
                pass

        threading.Thread(target=cleanup_mqtt, daemon=True).start()

        try:
            self.root.destroy()
        except Exception:
            import sys
            sys.exit(0)


if __name__ == "__main__":
    root = tk.Tk()
    app = ArduinoControllerApp(root)
    root.mainloop()