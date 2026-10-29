import base64
import datetime
import os
import socket
import struct
import sys
import threading
import tkinter as tk
from tkinter import messagebox, scrolledtext, ttk
import urllib.error
import urllib.parse
import urllib.request
import serial
import serial.tools.list_ports

try:
  from scapy.all import AsyncSniffer, get_if_list

  SCAPY_AVAILABLE = True
except ImportError:
  SCAPY_AVAILABLE = False


def compute_crc32(data: bytes) -> int:
  crc = 0xFFFFFFFF
  for byte in data:
    crc ^= (byte & 0xFF) << 24
    for _ in range(8):
      if crc & 0x80000000:
        crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
      else:
        crc = (crc << 1) & 0xFFFFFFFF
  return crc


class SerialEthernetMonitorApp:

  def __init__(self, root):
    self.root = root
    self.root.title(
        "Kodion Energy - Multi-CPU Transformer Command & Telemetry Test Center"
    )
    # Responsive sizing setup
    self.root.geometry("1350x950")
    self.root.minsize(900, 700)
    self.root.configure(bg="#121212")

    self.root.rowconfigure(0, weight=1)
    self.root.columnconfigure(0, weight=1)

    # Robust App Icon Loader for both Script mode and PyInstaller Executable (.exe)
    try:
      if getattr(sys, "frozen", False):
        application_path = sys._MEIPASS
      else:
        application_path = os.path.dirname(os.path.abspath(__file__))
      icon_path = os.path.join(application_path, "app_icon.ico")
      if os.path.exists(icon_path):
        self.root.iconbitmap(icon_path)
    except Exception as e:
      print(f"[Icon Loading Warning] {e}")

    # Apply High-End Industrial Dark Theme Styles
    self.apply_industrial_theme()

    # Default IP Configurations for 5 Connected CPUs
    self.cpu_units = {
        1: {
            "name": "Transformer Unit 1",
            "ip": "192.168.1.101",
            "header": "0xA0A0A0A0",
            "last_seen": 0,
            "data": {},
        },
        2: {
            "name": "Transformer Unit 2",
            "ip": "192.168.1.102",
            "header": "0xA0A0A0A1",
            "last_seen": 0,
            "data": {},
        },
        3: {
            "name": "Transformer Unit 3",
            "ip": "192.168.1.103",
            "header": "0xA0A0A0A2",
            "last_seen": 0,
            "data": {},
        },
        4: {
            "name": "Transformer Unit 4",
            "ip": "192.168.1.104",
            "header": "0xA0A0A0A3",
            "last_seen": 0,
            "data": {},
        },
        5: {
            "name": "Transformer Unit 5",
            "ip": "192.168.1.105",
            "header": "0xA0A0A0A4",
            "last_seen": 0,
            "data": {},
        },
    }

    # Main Master Container for Responsive Layout & Footer Integration
    self.master_container = ttk.Frame(self.root)
    self.master_container.grid(row=0, column=0, sticky="nsew")
    self.master_container.rowconfigure(0, weight=1)
    self.master_container.rowconfigure(1, weight=0)  # Footer row fixed at bottom
    self.master_container.columnconfigure(0, weight=1)

    # Main Tabbed Interface
    self.notebook = ttk.Notebook(self.master_container)
    self.notebook.grid(row=0, column=0, sticky="nsew", padx=8, pady=(8, 0))

    # 1. Serial Monitor Tab Container
    self.serial_frame = ttk.Frame(self.notebook)
    self.notebook.add(self.serial_frame, text=" Serial Monitor ")
    self.setup_serial_tab()

    # 2. Ethernet Packet Monitor Tab Container
    self.ethernet_frame = ttk.Frame(self.notebook)
    self.notebook.add(self.ethernet_frame, text=" Ethernet Packet Monitor ")
    self.setup_ethernet_tab()

    # 3. View / WebServer Tab Container
    self.webserver_frame = ttk.Frame(self.notebook)
    self.notebook.add(self.webserver_frame, text=" WebServer Inspector ")
    self.setup_webserver_tab()

    # 4. Transformer Server & Manager Tab Container (5 CPU Units)
    self.server_frame = ttk.Frame(self.notebook)
    self.notebook.add(
        self.server_frame, text=" ⚡ Transformer Cluster Server (5 CPUs) "
    )
    self.setup_server_tab()

    # Centralized Footer added to the bottom of the main application window
    footer_frame = ttk.Frame(self.master_container, padding=6)
    footer_frame.grid(row=1, column=0, sticky="ew", padx=8, pady=4)
    footer_frame.columnconfigure(0, weight=1)

    footer_label = tk.Label(
        footer_frame,
        text="Designed By Ubamadu Possible",
        bg="#121212",
        fg="#888888",
        font=("Segoe UI", 9, "italic"),
    )
    footer_label.grid(row=0, column=0)

    # State Variables
    self.serial_conn = None
    self.serial_thread = None
    self.serial_running = False

    self.sniffer = None
    self.sniffer_running = False
    self.captured_packets = []

    self.udp_telemetry_socket = None
    self.udp_time_socket = None
    self.server_running = False

    self.start_udp_servers()

  def apply_industrial_theme(self):
    style = ttk.Style()
    style.theme_use("clam")

    style.configure(
        ".",
        background="#121212",
        foreground="#ffffff",
        fieldbackground="#1e1e1e",
        troughcolor="#2a2a2a",
        font=("Segoe UI", 10),
    )
    style.configure(
        "TNotebook", background="#121212", borderwidth=0, tabmargins=[2, 5, 2, 0]
    )
    style.configure(
        "TNotebook.Tab",
        background="#1e1e1e",
        foreground="#cccccc",
        padding=[14, 8],
        font=("Segoe UI", 10, "bold"),
    )
    style.map(
        "TNotebook.Tab",
        background=[("selected", "#f39c12")],
        foreground=[("selected", "#121212")],
    )

    style.configure(
        "TLabelframe",
        background="#121212",
        foreground="#f39c12",
        bordercolor="#333333",
        borderwidth=1,
        relief="solid",
    )
    style.configure(
        "TLabelframe.Label",
        background="#121212",
        foreground="#f39c12",
        font=("Segoe UI", 10, "bold"),
    )
    style.configure(
        "TButton",
        background="#333333",
        foreground="#ffffff",
        borderwidth=1,
        focuscolor="none",
        padding=6,
    )
    style.map(
        "TButton",
        background=[("active", "#f39c12"), ("pressed", "#d35400")],
        foreground=[("active", "#121212"), ("pressed", "#ffffff")],
    )
    style.configure("Accent.TButton", background="#f39c12", foreground="#121212")
    style.map(
        "Accent.TButton",
        background=[("active", "#e67e22")],
        foreground=[("active", "#121212")],
    )

    style.configure(
        "TEntry",
        fieldbackground="#1e1e1e",
        foreground="#ffffff",
        insertcolor="#ffffff",
        bordercolor="#333333",
    )
    style.configure(
        "TCombobox",
        fieldbackground="#1e1e1e",
        background="#333333",
        foreground="#ffffff",
        bordercolor="#333333",
    )

    style.configure(
        "Treeview",
        background="#181818",
        foreground="#00bcd4",
        fieldbackground="#181818",
        bordercolor="#333333",
        rowheight=24,
    )
    style.configure(
        "Treeview.Heading",
        background="#252525",
        foreground="#f39c12",
        font=("Segoe UI", 9, "bold"),
    )
    style.map("Treeview", background=[("selected", "#005f73")])

  # ==================== SERIAL MONITOR TAB ====================
  def setup_serial_tab(self):
    self.serial_frame.rowconfigure(1, weight=1)
    self.serial_frame.columnconfigure(0, weight=1)

    control_frame = ttk.LabelFrame(
        self.serial_frame, text=" Serial Port Settings ", padding=12
    )
    control_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=8)

    ttk.Label(control_frame, text="Port:").grid(
        row=0, column=0, sticky="w", padx=5
    )
    self.port_combo = ttk.Combobox(control_frame, width=18, state="readonly")
    self.port_combo.grid(row=0, column=1, padx=5)
    self.refresh_ports()

    ttk.Button(
        control_frame, text="Refresh", command=self.refresh_ports
    ).grid(row=0, column=2, padx=5)

    ttk.Label(control_frame, text="Baudrate:").grid(
        row=0, column=3, sticky="w", padx=5
    )
    self.baud_combo = ttk.Combobox(control_frame, width=10, state="normal")
    self.baud_combo["values"] = [
        "9600",
        "19200",
        "38400",
        "57600",
        "115200",
        "230400",
        "460800",
        "921600",
    ]
    self.baud_combo.current(4)
    self.baud_combo.grid(row=0, column=4, padx=5)

    self.connect_btn = ttk.Button(
        control_frame,
        text="Connect",
        style="Accent.TButton",
        command=self.toggle_serial_connection,
    )
    self.connect_btn.grid(row=0, column=5, padx=10)

    ttk.Button(
        control_frame, text="Clear Log", command=self.clear_serial_log
    ).grid(row=0, column=6, padx=5)

    log_frame = ttk.LabelFrame(
        self.serial_frame, text=" Live Serial Data Stream ", padding=12
    )
    log_frame.grid(row=1, column=0, sticky="nsew", padx=10, pady=8)
    log_frame.rowconfigure(0, weight=1)
    log_frame.columnconfigure(0, weight=1)

    self.serial_log = scrolledtext.ScrolledText(
        log_frame,
        wrap=tk.WORD,
        state="disabled",
        bg="#181818",
        fg="#2ecc71",
        insertbackground="white",
        font=("Consolas", 10),
    )
    self.serial_log.grid(row=0, column=0, sticky="nsew", padx=5, pady=5)

    send_frame = ttk.Frame(self.serial_frame, padding=5)
    send_frame.grid(row=2, column=0, sticky="ew", padx=10, pady=5)
    send_frame.columnconfigure(1, weight=1)

    ttk.Label(send_frame, text="Transmit:").grid(row=0, column=0, sticky="w", padx=5)
    self.send_entry = ttk.Entry(send_frame, font=("Consolas", 10))
    self.send_entry.grid(row=0, column=1, sticky="ew", padx=5)
    self.send_entry.bind("<Return>", lambda event: self.send_serial_data())

    ttk.Button(
        send_frame,
        text="Send",
        style="Accent.TButton",
        command=self.send_serial_data,
    ).grid(row=0, column=2, sticky="e", padx=5)

  def refresh_ports(self):
    ports = [port.device for port in serial.tools.list_ports.comports()]
    self.port_combo["values"] = ports
    if ports:
      self.port_combo.current(0)

  def toggle_serial_connection(self):
    if not self.serial_running:
      port = self.port_combo.get()
      try:
        baud = int(self.baud_combo.get())
      except ValueError:
        messagebox.showerror("Error", "Invalid Baudrate value.")
        return
      if not port:
        messagebox.showerror("Error", "Please select a valid COM port.")
        return

      try:
        self.serial_conn = serial.Serial(port, baud, timeout=1)
        self.serial_running = True
        self.connect_btn.config(text="Disconnect")
        self.port_combo.config(state="disabled")
        self.baud_combo.config(state="disabled")

        self.serial_thread = threading.Thread(
            target=self.read_serial_loop, daemon=True
        )
        self.serial_thread.start()
        self.log_serial(f"[INFO] Connected to {port} at {baud} baud.\n")
      except Exception as e:
        messagebox.showerror("Connection Error", str(e))
    else:
      self.serial_running = False
      if self.serial_conn and self.serial_conn.is_open:
        self.serial_conn.close()
      self.connect_btn.config(text="Connect")
      self.port_combo.config(state="readonly")
      self.baud_combo.config(state="normal")
      self.log_serial("[INFO] Disconnected.\n")

  def read_serial_loop(self):
    while self.serial_running:
      try:
        if self.serial_conn and self.serial_conn.in_waiting > 0:
          data = self.serial_conn.read(
              self.serial_conn.in_waiting
          ).decode("utf-8", errors="replace")
          self.log_serial(data)
      except Exception:
        break

  def log_serial(self, text):
    self.serial_log.config(state="normal")
    self.serial_log.insert(tk.END, text)
    self.serial_log.see(tk.END)
    self.serial_log.config(state="disabled")

  def clear_serial_log(self):
    self.serial_log.config(state="normal")
    self.serial_log.delete("1.0", tk.END)
    self.serial_log.config(state="disabled")

  def send_serial_data(self):
    if self.serial_running and self.serial_conn and self.serial_conn.is_open:
      data = self.send_entry.get() + "\n"
      try:
        self.serial_conn.write(data.encode("utf-8"))
        self.log_serial(f">>> {data}")
        self.send_entry.delete(0, tk.END)
      except Exception as e:
        messagebox.showerror("Send Error", str(e))
    else:
      messagebox.showwarning("Warning", "Serial port is not connected.")

  # ==================== ETHERNET PACKET MONITOR TAB ====================
  def setup_ethernet_tab(self):
    self.ethernet_frame.rowconfigure(1, weight=3)
    self.ethernet_frame.rowconfigure(2, weight=1)
    self.ethernet_frame.columnconfigure(0, weight=1)

    control_frame = ttk.LabelFrame(
        self.ethernet_frame, text=" Network Capture Settings ", padding=12
    )
    control_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=8)

    ttk.Label(control_frame, text="Capture Interface:").grid(
        row=0, column=0, sticky="w", padx=5
    )
    self.iface_combo = ttk.Combobox(control_frame, width=28, state="readonly")
    self.iface_combo.grid(row=0, column=1, padx=5)

    if SCAPY_AVAILABLE:
      try:
        interfaces = get_if_list()
        self.iface_combo["values"] = interfaces
        if interfaces:
          self.iface_combo.current(0)
      except Exception:
        self.iface_combo["values"] = ["Default"]
        self.iface_combo.current(0)
    else:
      self.iface_combo["values"] = [
          "Native UDP Stream Capture (Scapy Missing)"
      ]
      self.iface_combo.current(0)

    self.sniff_btn = ttk.Button(
        control_frame,
        text="Start Sniffing / Stream Log",
        style="Accent.TButton",
        command=self.toggle_ethernet_sniffing,
    )
    self.sniff_btn.grid(row=0, column=2, padx=10)

    ttk.Button(
        control_frame, text="Clear Table", command=self.clear_packet_table
    ).grid(row=0, column=3, padx=5)

    if not SCAPY_AVAILABLE:
      notice_lbl = ttk.Label(
          control_frame,
          text=(
              "Note: Scapy is not installed. Native UDP Stream Capture is"
              " active, capturing all CPU telemetry packets automatically."
          ),
          foreground="#00bcd4",
      )
      notice_lbl.grid(row=1, column=0, columnspan=4, sticky="w", padx=5, pady=5)

    table_frame = ttk.LabelFrame(
        self.ethernet_frame, text=" Captured Network Packets (Expanded) ", padding=12
    )
    table_frame.grid(row=1, column=0, sticky="nsew", padx=10, pady=8)
    table_frame.rowconfigure(0, weight=1)
    table_frame.columnconfigure(0, weight=1)

    columns = ("no", "src", "dst", "size", "summary")
    self.packet_tree = ttk.Treeview(
        table_frame, columns=columns, show="headings", height=15
    )
    self.packet_tree.heading("no", text="No.")
    self.packet_tree.heading("src", text="Source IP / MAC")
    self.packet_tree.heading("dst", text="Destination IP / MAC")
    self.packet_tree.heading("size", text="Size (bytes)")
    self.packet_tree.heading("summary", text="Packet Summary / Payload")

    self.packet_tree.column("no", width=60, anchor="center")
    self.packet_tree.column("src", width=160, anchor="w")
    self.packet_tree.column("dst", width=160, anchor="w")
    self.packet_tree.column("size", width=90, anchor="center")
    self.packet_tree.column("summary", width=420, anchor="w")

    tree_scroll = ttk.Scrollbar(
        table_frame, orient="vertical", command=self.packet_tree.yview
    )
    self.packet_tree.configure(yscrollcommand=tree_scroll.set)

    self.packet_tree.grid(row=0, column=0, sticky="nsew")
    tree_scroll.grid(row=0, column=1, sticky="ns")

    self.packet_tree.bind("<<TreeviewSelect>>", self.on_packet_select)

    details_frame = ttk.LabelFrame(
        self.ethernet_frame, text=" Selected Packet Inspector (Compact) ", padding=12
    )
    details_frame.grid(row=2, column=0, sticky="nsew", padx=10, pady=8)
    details_frame.rowconfigure(1, weight=1)
    details_frame.columnconfigure(0, weight=1)

    format_bar = ttk.Frame(details_frame)
    format_bar.grid(row=0, column=0, sticky="ew", pady=4)

    ttk.Label(format_bar, text="Display Format:").pack(side="left", padx=5)
    self.format_combo = ttk.Combobox(
        format_bar,
        values=["HEX", "String", "Decimal", "Binary", "Base64"],
        width=12,
        state="readonly",
    )
    self.format_combo.current(0)
    self.format_combo.pack(side="left", padx=5)
    self.format_combo.bind("<<ComboboxSelected>>", lambda e: self.refresh_view())

    self.payload_view = scrolledtext.ScrolledText(
        details_frame,
        wrap=tk.WORD,
        state="normal",
        height=5,
        bg="#181818",
        fg="#00bcd4",
        insertbackground="white",
        font=("Consolas", 10),
    )
    self.payload_view.grid(row=1, column=0, sticky="nsew", pady=5)
    self.payload_view.config(state="disabled")

  def clear_packet_table(self):
    for item in self.packet_tree.get_children():
      self.packet_tree.delete(item)
    self.captured_packets.clear()
    self.payload_view.config(state="normal")
    self.payload_view.delete("1.0", tk.END)
    self.payload_view.config(state="disabled")

  def toggle_ethernet_sniffing(self):
    if not SCAPY_AVAILABLE:
      self.sniffer_running = not self.sniffer_running
      if self.sniffer_running:
        self.sniff_btn.config(text="Stop Stream Logging")
        messagebox.showinfo(
            "Native Stream Active",
            "Native UDP Stream monitoring is active. Incoming CPU telemetry and"
            " time sync packets are automatically populated here.",
        )
      else:
        self.sniff_btn.config(text="Start Sniffing / Stream Log")
      return

    if not self.sniffer_running:
      iface = self.iface_combo.get()
      if iface == "Default" or not iface:
        iface = None

      try:
        self.sniffer = AsyncSniffer(
            iface=iface, prn=self.packet_callback, store=False
        )
        self.sniffer.start()
        self.sniffer_running = True
        self.sniff_btn.config(text="Stop Sniffing")
        self.iface_combo.config(state="disabled")
      except Exception as e:
        messagebox.showerror(
            "Sniffer Error",
            f"{e}\n\nTip: Network interface sniffing requires Npcap / WinPcap"
            " on Windows or root privileges on Linux.",
        )
    else:
      try:
        if self.sniffer:
          self.sniffer.stop()
      except Exception:
        pass
      self.sniffer_running = False
      self.sniff_btn.config(text="Start Sniffing")
      self.iface_combo.config(state="readonly")

  def packet_callback(self, packet):
    if not self.sniffer_running:
      return
    self.root.after(0, lambda: self.process_packet_object(packet))

  def process_packet_object(self, packet):
    src = "N/A"
    dst = "N/A"
    if packet.haslayer("IP"):
      src = packet["IP"].src
      dst = packet["IP"].dst
    elif packet.haslayer("ARP"):
      src = packet["ARP"].psrc
      dst = packet["ARP"].pdst
    elif packet.haslayer("Ether"):
      src = packet["Ether"].src
      dst = packet["Ether"].dst

    pkt_size = len(packet)
    summary = packet.summary()

    pkt_id = len(self.captured_packets) + 1
    self.captured_packets.append(bytes(packet))

    self.packet_tree.insert(
        "", tk.END, values=(pkt_id, src, dst, pkt_size, summary)
    )
    children = self.packet_tree.get_children()
    if children:
      self.packet_tree.see(children[-1])

  def log_raw_udp_packet(self, src_ip, raw_bytes, summary_text):
    if not self.sniffer_running and SCAPY_AVAILABLE:
      return
    if not self.sniffer_running and not SCAPY_AVAILABLE:
      return

    pkt_id = len(self.captured_packets) + 1
    self.captured_packets.append(raw_bytes)

    self.packet_tree.insert(
        "",
        tk.END,
        values=(
            pkt_id,
            src_ip,
            "192.168.1.100 (Host Server)",
            len(raw_bytes),
            summary_text,
        ),
    )
    children = self.packet_tree.get_children()
    if children:
      self.packet_tree.see(children[-1])

  def on_packet_select(self, event):
    self.refresh_view()

  def refresh_view(self):
    selected_items = self.packet_tree.selection()
    if not selected_items:
      return

    item_id = selected_items[0]
    idx = self.packet_tree.index(item_id)
    if idx < len(self.captured_packets):
      raw_bytes = self.captured_packets[idx]
      fmt = self.format_combo.get()

      formatted_text = ""
      if fmt == "HEX":
        hex_lines = []
        for i in range(0, len(raw_bytes), 16):
          chunk = raw_bytes[i : i + 16]
          hex_str = " ".join(f"{b:02X}" for b in chunk)
          ascii_str = "".join(
              chr(b) if 32 <= b <= 126 else "." for b in chunk
          )
          hex_lines.append(f"{i:04X}   {hex_str:<48}   {ascii_str}")
        formatted_text = "\n".join(hex_lines)
      elif fmt == "String":
        formatted_text = raw_bytes.decode("utf-8", errors="replace")
      elif fmt == "Decimal":
        formatted_text = " ".join(str(b) for b in raw_bytes)
      elif fmt == "Binary":
        formatted_text = " ".join(format(b, "08b") for b in raw_bytes)
      elif fmt == "Base64":
        formatted_text = base64.b64encode(raw_bytes).decode("utf-8")

      self.payload_view.config(state="normal")
      self.payload_view.delete("1.0", tk.END)
      self.payload_view.insert(tk.END, formatted_text)
      self.payload_view.config(state="disabled")

  # ==================== VIEW / WEBSERVER TAB ====================
  def setup_webserver_tab(self):
    self.webserver_frame.rowconfigure(1, weight=1)
    self.webserver_frame.columnconfigure(0, weight=1)

    url_frame = ttk.LabelFrame(
        self.webserver_frame, text=" HTTP WebServer Inspector ", padding=12
    )
    url_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=8)
    url_frame.columnconfigure(1, weight=1)

    ttk.Label(url_frame, text="Target CPU URL:").grid(
        row=0, column=0, sticky="w", padx=5
    )
    self.url_entry = ttk.Entry(url_frame, font=("Consolas", 10))
    self.url_entry.insert(0, "http://192.168.1.101")
    self.url_entry.grid(row=0, column=1, sticky="ew", padx=5)
    self.url_entry.bind(
        "<Return>",
        lambda event: threading.Thread(target=self.fetch_webpage).start(),
    )

    ttk.Button(
        url_frame,
        text="Fetch GET",
        style="Accent.TButton",
        command=lambda: threading.Thread(target=self.fetch_webpage).start(),
    ).grid(row=0, column=2, padx=5)

    ttk.Button(
        url_frame, text="Open in Browser", command=self.open_in_browser
    ).grid(row=0, column=3, padx=5)

    resp_frame = ttk.LabelFrame(
        self.webserver_frame, text=" HTTP Response & HTML Payload ", padding=12
    )
    resp_frame.grid(row=1, column=0, sticky="nsew", padx=10, pady=8)
    resp_frame.rowconfigure(2, weight=1)
    resp_frame.columnconfigure(0, weight=1)

    self.status_label = ttk.Label(
        resp_frame, text="Status: Ready", font=("Segoe UI", 10, "bold")
    )
    self.status_label.grid(row=0, column=0, sticky="w", padx=5, pady=2)

    ttk.Label(resp_frame, text="Response Body:").grid(
        row=1, column=0, sticky="w", padx=5, pady=2
    )
    self.web_response_view = scrolledtext.ScrolledText(
        resp_frame,
        wrap=tk.WORD,
        state="normal",
        bg="#181818",
        fg="#ffffff",
        insertbackground="white",
        font=("Consolas", 10),
    )
    self.web_response_view.grid(row=2, column=0, sticky="nsew", padx=5, pady=5)
    self.web_response_view.config(state="disabled")

  def fetch_webpage(self):
    url = self.url_entry.get().strip()
    if not url.startswith("http://") and not url.startswith("https://"):
      url = "http://" + url

    self.root.after(
        0,
        lambda: self.status_label.config(
            text=f"Status: Connecting to {url}..."
        ),
    )

    try:
      req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
      with urllib.request.urlopen(req, timeout=5) as response:
        status = response.getcode()
        headers = dict(response.headers)
        body = response.read().decode("utf-8", errors="replace")

        header_str = "\n".join(f"{k}: {v}" for k, v in headers.items())
        full_content = (
            f"=== HTTP HEADERS ===\n{header_str}\n\n=== HTTP BODY ===\n{body}"
        )

        self.root.after(
            0,
            lambda: self.update_web_response(
                f"Status: HTTP {status} OK", full_content
            ),
        )
    except urllib.error.HTTPError as e:
      error_body = e.read().decode("utf-8", errors="replace")
      self.root.after(
          0,
          lambda: self.update_web_response(
              f"Status: HTTP Error {e.code}", f"Error Body:\n{error_body}"
          ),
      )
    except Exception as e:
      self.root.after(
          0, lambda: self.update_web_response("Status: Connection Failed", str(e))
      )

  def update_web_response(self, status_text, content):
    self.status_label.config(text=status_text)
    self.web_response_view.config(state="normal")
    self.web_response_view.delete("1.0", tk.END)
    self.web_response_view.insert(tk.END, content)
    self.web_response_view.config(state="disabled")

  def open_in_browser(self):
    import webbrowser

    url = self.url_entry.get().strip()
    if not url.startswith("http://") and not url.startswith("https://"):
      url = "http://" + url
    webbrowser.open(url)

  # ==================== TRANSFORMER CLUSTER SERVER & MANAGER TAB ====================
  def setup_server_tab(self):
    self.server_frame.rowconfigure(0, weight=1)
    self.server_frame.columnconfigure(0, weight=1)

    self.server_notebook = ttk.Notebook(self.server_frame)
    self.server_notebook.grid(row=0, column=0, sticky="nsew", padx=5, pady=5)

    # Overview Sub-Tab
    self.overview_frame = ttk.Frame(self.server_notebook)
    self.server_notebook.add(self.overview_frame, text=" Cluster Overview ")
    self.setup_overview_subtab()

    # Create Unit Sub-Tabs for CPU Unit #1 to Unit #5
    self.unit_ui = {}
    for unit_id in range(1, 6):
      unit_frame = ttk.Frame(self.server_notebook)
      self.server_notebook.add(
          unit_frame, text=f" CPU Unit #{unit_id} Setup "
      )
      self.setup_unit_tab(unit_id, unit_frame)

  def setup_overview_subtab(self):
    self.overview_frame.rowconfigure(1, weight=1)
    self.overview_frame.columnconfigure(0, weight=1)

    header_frame = ttk.LabelFrame(
        self.overview_frame, text=" UDP Server Daemon Status ", padding=12
    )
    header_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=8)

    self.server_status_lbl = ttk.Label(
        header_frame,
        text=(
            "Telemetry Server: Port 5000 (Active) | Time Sync Server: Port 5007"
            " (Active - Auto Responding to AA 01)"
        ),
        font=("Segoe UI", 10, "bold"),
        foreground="#2ecc71",
    )
    self.server_status_lbl.pack(side="left", padx=5)

    cards_container = ttk.Frame(self.overview_frame, padding=5)
    cards_container.grid(row=1, column=0, sticky="nsew", padx=5, pady=5)
    cards_container.rowconfigure(0, weight=1)
    cards_container.rowconfigure(1, weight=1)
    cards_container.columnconfigure(0, weight=1)
    cards_container.columnconfigure(1, weight=1)
    cards_container.columnconfigure(2, weight=1)

    self.unit_cards = {}
    for unit_id in range(1, 6):
      row = (unit_id - 1) // 3
      col = (unit_id - 1) % 3

      card = ttk.LabelFrame(
          cards_container,
          text=f"CPU #{unit_id} ({self.cpu_units[unit_id]['ip']})",
          padding=10,
      )
      card.grid(row=row, column=col, padx=8, pady=8, sticky="nsew")
      card.rowconfigure(1, weight=1)
      card.columnconfigure(0, weight=1)

      status_lbl = ttk.Label(
          card,
          text="STATUS: OFFLINE",
          font=("Segoe UI", 9, "bold"),
          foreground="#e74c3c",
      )
      status_lbl.grid(row=0, column=0, sticky="w", pady=2)

      metrics_text = scrolledtext.ScrolledText(
          card,
          state="disabled",
          bg="#181818",
          fg="#00bcd4",
          font=("Consolas", 9),
      )
      metrics_text.grid(row=1, column=0, sticky="nsew", pady=4)

      self.unit_cards[unit_id] = {
          "status": status_lbl,
          "metrics": metrics_text,
      }

  def setup_unit_tab(self, unit_id, frame):
    frame.rowconfigure(1, weight=1)
    frame.columnconfigure(0, weight=1)

    self.unit_ui[unit_id] = {}

    cfg_frame = ttk.LabelFrame(
        frame, text=" Unit Network Identification ", padding=12
    )
    cfg_frame.grid(row=0, column=0, sticky="ew", padx=10, pady=8)

    ttk.Label(cfg_frame, text="IP Address:").grid(
        row=0, column=0, sticky="w", padx=5
    )
    ip_entry = ttk.Entry(cfg_frame, width=18, font=("Consolas", 10))
    ip_entry.insert(0, self.cpu_units[unit_id]["ip"])
    ip_entry.grid(row=0, column=1, padx=5)

    ttk.Label(cfg_frame, text="Device Header (HEX):").grid(
        row=0, column=2, sticky="w", padx=5
    )
    hdr_entry = ttk.Entry(cfg_frame, width=18, font=("Consolas", 10))
    hdr_entry.insert(0, self.cpu_units[unit_id]["header"])
    hdr_entry.grid(row=0, column=3, padx=5)

    def save_cfg():
      self.cpu_units[unit_id]["ip"] = ip_entry.get().strip()
      self.cpu_units[unit_id]["header"] = hdr_entry.get().strip()
      messagebox.showinfo(
          "Saved", f"Updated Network Config for CPU Unit #{unit_id}"
      )

    ttk.Button(cfg_frame, text="Save Config", command=save_cfg).grid(
        row=0, column=4, padx=10
    )

    ctrl_container = ttk.Frame(frame, padding=5)
    ctrl_container.grid(row=1, column=0, sticky="nsew", padx=5, pady=5)
    ctrl_container.rowconfigure(0, weight=1)
    ctrl_container.columnconfigure(0, weight=1)
    ctrl_container.columnconfigure(1, weight=1)

    # Column 1: Cooling Mode & Relays
    col1 = ttk.LabelFrame(
        ctrl_container, text=" Cooling Logic & Relays ", padding=12
    )
    col1.grid(row=0, column=0, sticky="nsew", padx=5, pady=5)

    ttk.Label(col1, text="Cooling Mode Control:").pack(
        anchor="w", pady=2, fill="x"
    )
    ttk.Button(
        col1,
        text="Switch to MANUAL Mode",
        command=lambda u=unit_id: self.send_http_cmd(
            u, "/api/set_cooling_mode", {"mode": "manual"}
        ),
    ).pack(fill="x", pady=3)
    ttk.Button(
        col1,
        text="Switch to AUTOMATIC Mode",
        command=lambda u=unit_id: self.send_http_cmd(
            u, "/api/set_cooling_mode", {"mode": "auto"}
        ),
    ).pack(fill="x", pady=3)

    ttk.Separator(col1, orient="horizontal").pack(fill="x", pady=12)

    ttk.Label(col1, text="Relay Manual Overrides:").pack(
        anchor="w", pady=2, fill="x"
    )
    ttk.Button(
        col1,
        text="Stage A: ON",
        command=lambda u=unit_id: self.send_http_cmd(
            u, "/api/set_stage_override", {"stage": "A", "state": "1"}
        ),
    ).pack(fill="x", pady=2)
    ttk.Button(
        col1,
        text="Stage A: OFF",
        command=lambda u=unit_id: self.send_http_cmd(
            u, "/api/set_stage_override", {"stage": "A", "state": "0"}
        ),
    ).pack(fill="x", pady=2)
    ttk.Button(
        col1,
        text="Stage B: ON",
        command=lambda u=unit_id: self.send_http_cmd(
            u, "/api/set_stage_override", {"stage": "B", "state": "1"}
        ),
    ).pack(fill="x", pady=2)
    ttk.Button(
        col1,
        text="Stage B: OFF",
        command=lambda u=unit_id: self.send_http_cmd(
            u, "/api/set_stage_override", {"stage": "B", "state": "0"}
        ),
    ).pack(fill="x", pady=2)

    # Column 2: Timers, PF & System Controls
    col2 = ttk.LabelFrame(
        ctrl_container, text=" Timers, PF & System Controls ", padding=12
    )
    col2.grid(row=0, column=1, sticky="nsew", padx=5, pady=5)

    ttk.Label(col2, text="Power Factor (0.0 - 1.0):").pack(
        anchor="w", pady=2, fill="x"
    )
    pf_entry = ttk.Entry(col2, font=("Consolas", 10))
    pf_entry.insert(0, "0.85")
    pf_entry.pack(fill="x", pady=3)
    ttk.Button(
        col2,
        text="Save Power Factor",
        command=lambda u=unit_id, e=pf_entry: self.send_http_cmd(
            u, "/set_pf", {"pf": e.get().strip()}
        ),
    ).pack(fill="x", pady=3)

    ttk.Separator(col2, orient="horizontal").pack(fill="x", pady=10)

    ttk.Label(col2, text="Compressor ON/OFF Timers (Min):").pack(
        anchor="w", pady=2, fill="x"
    )
    on_entry = ttk.Entry(col2, font=("Consolas", 10))
    on_entry.insert(0, "15")
    on_entry.pack(fill="x", pady=2)
    off_entry = ttk.Entry(col2, font=("Consolas", 10))
    off_entry.insert(0, "10")
    off_entry.pack(fill="x", pady=2)
    ttk.Button(
        col2,
        text="Save Timers",
        command=lambda u=unit_id, e1=on_entry, e2=off_entry: self.send_http_cmd(
            u,
            "/set_timers",
            {"ontime": e1.get().strip(), "offtime": e2.get().strip()},
        ),
    ).pack(fill="x", pady=3)

    ttk.Separator(col2, orient="horizontal").pack(fill="x", pady=10)

    ttk.Button(
        col2,
        text="REBOOT CPU",
        style="Accent.TButton",
        command=lambda u=unit_id: self.send_udp_cmd(u, "000001"),
    ).pack(fill="x", pady=4)

  # ==================== UDP SERVERS & THREADS ====================
  def start_udp_servers(self):
    self.server_running = True
    threading.Thread(target=self.udp_telemetry_loop, daemon=True).start()
    threading.Thread(target=self.udp_time_loop, daemon=True).start()
    threading.Thread(target=self.ui_heartbeat_loop, daemon=True).start()

  def udp_telemetry_loop(self):
    try:
      self.udp_telemetry_socket = socket.socket(
          socket.AF_INET, socket.SOCK_DGRAM
      )
      self.udp_telemetry_socket.bind(("0.0.0.0", 5000))
      while self.server_running:
        data, addr = self.udp_telemetry_socket.recvfrom(1024)
        if len(data) == 64:
          self.parse_telemetry_packet(data, addr[0])
    except Exception as e:
      print(f"[UDP Telemetry Error] {e}")

  def parse_telemetry_packet(self, data: bytes, sender_ip: str):
    header = struct.unpack(">I", data[0:4])[0]
    fields = struct.unpack(">18h", data[8:44])

    def decode_val(raw_val):
      return "FAULT" if raw_val == -32768 else f"{raw_val / 100.0:.2f}"

    telemetry = {
        "oil": decode_val(fields[0]),
        "wind": decode_val(fields[1]),
        "tank": decode_val(fields[2]),
        "amb": decode_val(fields[3]),
        "stgA_t": decode_val(fields[4]),
        "stgB_t": decode_val(fields[5]),
        "stgA_p": decode_val(fields[6]),
        "stgB_p": decode_val(fields[7]),
        "v1": decode_val(fields[8]),
        "v2": decode_val(fields[9]),
        "v3": decode_val(fields[10]),
        "c1": decode_val(fields[11]),
        "c2": decode_val(fields[12]),
        "c3": decode_val(fields[13]),
        "real_p": decode_val(fields[14]),
        "react_p": decode_val(fields[15]),
        "app_p": decode_val(fields[16]),
        "pf": decode_val(fields[17]),
    }

    summary_str = (
        f"CPU Telemetry [Oil: {telemetry['oil']}°C, Wind:"
        f" {telemetry['wind']}°C, Pwr: {telemetry['real_p']}kW]"
    )
    self.root.after(
        0, lambda: self.log_raw_udp_packet(sender_ip, data, summary_str)
    )

    for unit_id, config in self.cpu_units.items():
      if config["ip"] == sender_ip or hex(header).lower() == config[
          "header"
      ].lower():
        config["last_seen"] = datetime.datetime.now().timestamp()
        config["data"] = telemetry
        self.root.after(0, lambda u=unit_id: self.update_unit_ui(u))
        break

  def udp_time_loop(self):
    """Listens on Port 5007 for AA 01 requests and responds with date/time packet."""
    try:
      self.udp_time_socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
      self.udp_time_socket.bind(("0.0.0.0", 5007))
      while self.server_running:
        data, addr = self.udp_time_socket.recvfrom(128)
        if len(data) == 2 and data[0] == 0xAA and data[1] == 0x01:
          now = datetime.datetime.now()
          resp = struct.pack(
              ">BBHBBBBB",
              0xAA,
              0x00,
              now.year,
              now.month,
              now.day,
              now.hour,
              now.minute,
              now.second,
          )
          self.udp_time_socket.sendto(resp, addr)
          self.root.after(
              0,
              lambda: self.log_raw_udp_packet(
                  addr[0],
                  data,
                  f"Received AA 01 Time Request -> Sent Date/Time Packet:"
                  f" {now.strftime('%Y-%m-%d %H:%M:%S')}",
              ),
          )
    except Exception as e:
      print(f"[UDP Time Sync Error] {e}")

  def update_unit_ui(self, unit_id):
    card = self.unit_cards[unit_id]
    data = self.cpu_units[unit_id]["data"]

    card["status"].config(text="STATUS: ONLINE", foreground="#2ecc71")

    text = (
        f"Oil Temp:    {data.get('oil')} °C\n"
        f"Winding Temp:{data.get('wind')} °C\n"
        f"Tank Temp:   {data.get('tank')} °C\n"
        f"Press. A:    {data.get('stgA_p')} PSI\n"
        f"Press. B:    {data.get('stgB_p')} PSI\n"
        f"Voltages:    {data.get('v1')}V / {data.get('v2')}V /"
        f" {data.get('v3')}V\n"
        f"Currents:    {data.get('c1')}A / {data.get('c2')}A /"
        f" {data.get('c3')}A\n"
        f"Real Power:  {data.get('real_p')} kW\n"
        f"Power Fact.: {data.get('pf')}"
    )

    card["metrics"].config(state="normal")
    card["metrics"].delete("1.0", tk.END)
    card["metrics"].insert(tk.END, text)
    card["metrics"].config(state="disabled")

  def ui_heartbeat_loop(self):
    while self.server_running:
      now = datetime.datetime.now().timestamp()
      for unit_id, config in self.cpu_units.items():
        if now - config["last_seen"] > 6:
          self.root.after(0, lambda u=unit_id: self.mark_offline(u))
      threading.Event().wait(2.0)

  def mark_offline(self, unit_id):
    card = self.unit_cards[unit_id]
    card["status"].config(text="STATUS: OFFLINE", foreground="#e74c3c")

  def send_http_cmd(self, unit_id, path, params):
    def task():
      ip = self.cpu_units[unit_id]["ip"]
      url = f"http://{ip}{path}?" + urllib.parse.urlencode(params)
      try:
        req = urllib.request.Request(
            url, method="POST" if "api" in path else "GET"
        )
        with urllib.request.urlopen(req, timeout=3) as resp:
          res_text = resp.read().decode("utf-8")
          self.root.after(
              0,
              lambda: messagebox.showinfo(
                  "Success", f"CPU Unit #{unit_id} Response:\n{res_text}"
              ),
          )
      except Exception as e:
        self.root.after(
            0,
            lambda: messagebox.showerror(
                "Error", f"Failed to send HTTP command to Unit #{unit_id}:\n{e}"
            ),
        )

    threading.Thread(target=task, daemon=True).start()

  def send_udp_cmd(self, unit_id, cmd_str):
    ip = self.cpu_units[unit_id]["ip"]
    try:
      sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
      sock.sendto(cmd_str.encode("utf-8"), (ip, 1234))
      messagebox.showinfo(
          "Sent", f"Sent UDP Command '{cmd_str}' to Unit #{unit_id} ({ip}:1234)"
      )
    except Exception as e:
      messagebox.showerror("Error", str(e))


if __name__ == "__main__":
  root = tk.Tk()
  app = SerialEthernetMonitorApp(root)
  root.mainloop()