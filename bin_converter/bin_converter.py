import os
import re
import subprocess
import shutil
import tkinter as tk
from tkinter import filedialog, messagebox

try:
    import paramiko
except ImportError:
    paramiko = None

file_name = ""  # updated by upload_file() via `global`


def upload_file():
    global file_name
    file_path = filedialog.askopenfilename(
        title="Select a file",
        filetypes=[("Binary Files", "*.bin"), ("All files", "*.*")]
    )
    if not file_path:
        return

    compfile = "compressed_file.lzss"
    shutil.copy2(file_path, './')
    file_name = os.path.basename(file_path)

    result = subprocess.run(
        ['python', 'lzconvert.py', '--encode', file_name, compfile],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        messagebox.showerror("Compression failed", result.stderr or "Unknown error")
        upload_status.set("Compression failed")
    else:
        upload_status.set(f"Uploaded & compressed: {file_name}")


def convert_file():
    if not file_name:
        messagebox.showwarning("No file", "Upload a .bin file first.")
        return

    dev_type = device_type_var.get().strip()
    ota_name = ota_name_var.get().strip()

    if not dev_type or not ota_name:
        messagebox.showwarning("Missing info", "Enter both device type and OTA file name.")
        return

    result = subprocess.run(
        ['python', 'bin2ota.py', dev_type, "compressed_file.lzss", ota_name],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        messagebox.showerror("Conversion failed", result.stderr or "Unknown error")
        convert_status.set("Conversion failed")
    else:
        convert_status.set(f"Converted: {ota_name}")


def parse_destination(dest_str):
    """
    Parses strings like:
      scp username@1.2.3.4:8080/opt/entgen/firmware/filename
    Returns (username, host, port, remote_path) or None if it doesn't match.
    """
    dest_str = dest_str.strip()
    if dest_str.lower().startswith("scp "):
        dest_str = dest_str[4:].strip()

    match = re.match(r'^([^@\s]+)@([^:\s]+):(\d+)?(/\S*)$', dest_str)
    if not match:
        return None
    username, host, port, path = match.groups()
    port = int(port) if port else 22
    return username, host, port, path


def send_file():
    if paramiko is None:
        messagebox.showerror(
            "Missing dependency",
            "paramiko is not installed. Run:\n\npip install paramiko"
        )
        return

    dest_str = destination.get("1.0", "end-1c")
    pwd = password.get("1.0", "end-1c")

    parsed = parse_destination(dest_str)
    if not parsed:
        messagebox.showerror(
            "Invalid destination",
            "Use the format: scp username@IP:8080/opt/entgen/firmware/filename"
        )
        return

    username, host, port, remote_path = parsed
    ota_name = ota_name_var.get().strip()

    if not ota_name or not os.path.exists(ota_name):
        messagebox.showerror("File not found", f"Could not find local OTA file: {ota_name}")
        return

    try:
        transport = paramiko.Transport((host, port))
        transport.connect(username=username, password=pwd)
        sftp = paramiko.SFTPClient.from_transport(transport)
        sftp.put(ota_name, remote_path)
        sftp.close()
        transport.close()
        messagebox.showinfo("Success", f"Sent {ota_name} to {host}:{remote_path}")
    except Exception as e:
        messagebox.showerror("Send failed", str(e))


root = tk.Tk()
root.title("Bin to OTA Converter")
root.minsize(500, 500)

device_type_var = tk.StringVar()
ota_name_var = tk.StringVar()
upload_status = tk.StringVar(value="No file uploaded yet")
convert_status = tk.StringVar(value="Not converted yet")

tk.Label(root, text="Welcome to Bin to OTA Converter").pack(pady=(10, 0))
tk.Label(root, text="Upload your Arduino Binary file, name it and hit convert!").pack()

tk.Label(root, text="STEP 1: UPLOAD YOUR BINARY").pack(pady=(15, 0))
tk.Button(root, text="UPLOAD FILE", command=upload_file).pack()
tk.Label(root, textvariable=upload_status, fg="gray").pack()

tk.Label(root, text="STEP 2: CONVERT IT!").pack(pady=(15, 0))
tk.Label(root, text="Device Type").pack()
tk.Entry(root, textvariable=device_type_var).pack()

tk.Label(root, text="OTA File Name").pack()
tk.Entry(root, textvariable=ota_name_var).pack()

tk.Button(root, text="CONVERT", command=convert_file).pack()
tk.Label(root, textvariable=convert_status, fg="gray").pack()

tk.Label(root, text="STEP 3: SEND IT TO THE BROKER").pack(pady=(15, 0))
tk.Label(root, text="Must be in [scp username@IP:8080/opt/entgen/firmware/filename] format").pack()
destination = tk.Text(root, height=2, width=40)
destination.pack()

tk.Label(root, text="Broker Password (passed through, not saved)").pack()
password = tk.Text(root, height=1, width=40)
password.pack()

tk.Button(root, text="SEND", command=send_file).pack(pady=(5, 15))

root.mainloop()