import json
import os
import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext, ttk
import paho.mqtt.client as mqtt

CONFIG_FILE = "esp_mqtt_config.json"


class ConfigWindow(tk.Toplevel):
    """Dedicated Configuration Window for MQTT Broker & Topics"""

    def __init__(self, parent, current_config, on_save_callback):
        super().__init__(parent)
        self.title("MQTT Broker Settings")
        self.geometry("450x450")
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
            text="EMQX / MQTT Broker Configuration",
            font=("Arial", 12, "bold"),
        ).pack(anchor="w", pady=(0, 10))

        form_frame = ttk.LabelFrame(
            main_frame, text="Connection Parameters", padding=10
        )
        form_frame.pack(fill="both", expand=True, pady=5)

        self.entries = {}
        fields = [
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
            row_frame.pack(fill="x", pady=6)

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
            text="Save & Reconnect",
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
            if key == "mqtt_port":
                try:
                    val = int(val)
                except ValueError:
                    val = 8883
            new_config[key] = val

        self.on_save_callback(new_config)
        messagebox.showinfo(
            "Success", "Configuration saved successfully!", parent=self
        )
        self.destroy()


class ESP32MQTTControllerApp:

    def __init__(self, root):
        self.root = root
        self.root.title("ESP32 MQTT Home Automation Controller")
        self.root.geometry("620x640")
        self.root.resizable(False, False)

        self.relay_states = [False, False, False, False]
        self.relay_buttons = []
        self.last_sent_dimmer = 0
        self.is_mqtt_connected = False

        # Load configurations
        self.config = self.load_config()

        self.create_widgets()

        # Initialize MQTT Client in background thread to avoid startup hanging
        threading.Thread(target=self.init_mqtt_client, daemon=True).start()

        # Clean window close handler
        self.root.protocol("WM_DELETE_WINDOW", self.on_closing)

    def load_config(self):
        default_config = {
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
            self.save_config_to_disk()

            if hasattr(self, "mqtt_client"):
                try:
                    self.mqtt_client.loop_stop()
                    self.mqtt_client.disconnect()
                except Exception:
                    pass
            self.init_mqtt_client()
            self.safe_log(">> MQTT Configuration updated and reconnecting...\n")

        threading.Thread(target=background_update, daemon=True).start()

    def init_mqtt_client(self):
        broker = self.config.get("mqtt_server")
        port = self.config.get("mqtt_port")
        user = self.config.get("mqtt_user")
        passwd = self.config.get("mqtt_pass")

        self.mqtt_client = mqtt.Client(
            client_id="tkinter_pc_controller",
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            protocol=mqtt.MQTTv5,
        )
        self.mqtt_client.username_pw_set(username=user, password=passwd)
        self.mqtt_client.tls_set()  # Required for secure EMQX cloud broker
        self.mqtt_client.on_connect = self.on_mqtt_connect
        self.mqtt_client.on_message = self.on_mqtt_message

        try:
            self.safe_log(f">> Connecting to broker {broker}:{port}...\n")
            self.mqtt_client.connect_async(broker, int(port), keepalive=60)
            self.mqtt_client.loop_start()
        except Exception as e:
            self.safe_log(f"MQTT Connection Error: {e}\n")

    def open_config_window(self):
        ConfigWindow(
            self.root, self.config, self.update_config_from_dialog
        )

    def create_widgets(self):
        # Top Header Bar with Settings Config button
        top_bar = ttk.Frame(self.root)
        top_bar.pack(fill="x", padx=10, pady=(10, 0))

        title_lbl = ttk.Label(
            top_bar, text="ESP32 MQTT Home Automation", font=("Arial", 12, "bold")
        )
        title_lbl.pack(side="left", padx=5)

        config_btn = tk.Button(
            top_bar,
            text="⚙️ MQTT Config",
            bg="#6c757d",
            fg="white",
            font=("Arial", 9, "bold"),
            relief="flat",
            command=self.open_config_window,
        )
        config_btn.pack(side="right", padx=5)

        # --- CONNECTION STATUS BANNER ---
        status_frame = ttk.LabelFrame(
            self.root, text="Broker Connection Status", padding=10
        )
        status_frame.pack(fill="x", padx=10, pady=10)

        self.status_lbl = tk.Label(
            status_frame,
            text="Status: Connecting...",
            font=("Arial", 10, "bold"),
            bg="#ffebee",
            fg="#c62828",
            padx=10,
            pady=5,
        )
        self.status_lbl.pack(fill="x")

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
            self.root, text="MQTT Console / Live Feedback", padding=10
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
            self.is_mqtt_connected = True
            self.safe_update_status(True)
            self.safe_log("Connected to EMQX MQTT Broker successfully.\n")

            t_state = self.config.get(
                "mqtt_topic_state", "home/esp8266/state"
            )
            client.subscribe(t_state)

            # Automatically request initial state upon connection
            self.send_command("STATUS")
        else:
            self.is_mqtt_connected = False
            self.safe_update_status(False)
            self.safe_log(
                f"MQTT Connection failed with return code {reason_code}\n"
            )

    def on_mqtt_message(self, client, userdata, msg):
        try:
            payload_str = msg.payload.decode("utf-8", errors="ignore")
            self.safe_log(f"<< [State Sync]: {payload_str}\n")
            self.parse_esp_feedback(payload_str)
        except Exception as e:
            print(f"Message parse error: {e}")

    def safe_update_status(self, connected):
        def update():
            if connected:
                self.status_lbl.config(
                    text="Status: Connected to MQTT Broker",
                    bg="#e8f5e9",
                    fg="#2e7d32",
                )
            else:
                self.status_lbl.config(
                    text="Status: Disconnected / Reconnecting...",
                    bg="#ffebee",
                    fg="#c62828",
                )

        self.root.after(0, update)

    def toggle_relay(self, relay_num):
        idx = relay_num - 1
        current_state = self.relay_states[idx]
        target_state = not current_state
        cmd = f"POWER{relay_num} {'ON' if target_state else 'OFF'}"
        self.send_command(cmd)

    def send_command(self, cmd):
        try:
            t_cmd = self.config.get("mqtt_topic_cmd", "home/esp8266/command")
            mqtt_payload = json.dumps({"command": cmd})
            self.mqtt_client.publish(t_cmd, mqtt_payload)
            self.log_message(f">> Published Command: {cmd}\n")
        except Exception as e:
            self.log_message(f">> Publish Failed: {e}\n")

    def on_slider_release(self, event):
        val_int = int(round(self.dimmer_slider.get()))
        self.dimmer_slider.set(val_int)
        if val_int != self.last_sent_dimmer:
            self.last_sent_dimmer = val_int
            self.send_command(f"DIMMER {val_int}")

    def parse_esp_feedback(self, payload):
        try:
            data = json.loads(payload)
            # Handle state_update format broadcasted by ESP32
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
        """Clean up background threads instantly on exit without freezing"""
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
    app = ESP32MQTTControllerApp(root)
    root.protocol("WM_DELETE_WINDOW", app.on_closing)
    root.mainloop()