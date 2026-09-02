import os
import subprocess
import tkinter as tk
from tkinter import filedialog

file_name = ""

def upload_file():
	file_path = filedialog.askopenfilename(title="select a file",filetypes=[("Binary Files","*.bin"), ("All files", "*.*")])
	compfile = "compressed_file.lzss"
	if file_path:
		shutil.copy2(file_path, './')
		file_name = os.path.basename(file_path)
		
		subprocess.run(['python3','lzss.py', '--encode', file_name, compfile)


root = tk.Tk()

root.title("Bin to OTA Converter")
root.minsize(500,500)
tk.Label(root,text="Welcome to Bin to OTA Converter").pack()
tk.Label(root,text="Upload your Arduino Binary file, name it and hit convert!").pack()
tk.Label(root,text= "STEP 1: UPLOAD YOUR BINARY").pack()
upload_button = tk.Button(root, text="UPLOAD FILE",command=upload_file)
upload_button.pack()
tk.Label(root,text=file_path).pack()
tk.Label(root,text="STEP 2: NAME THE OUTPUT").pack()
filename = tk.Text(root, height=2, width=40)
filename.pack()

tk.Label(root,text="STEP 3: CONVERT IT!").pack()
convert_button = tk.Button(root, text="CONVERT")
convert_button.pack()

tk.Label(root,text="STEP 4: SEND IT TO THE BROKER").pack()
tk.Label(root,text="Must be in [scp username@IP:8080/opt/entgen/firmware/filename] format").pack()
destination= tk.Text(root,height=2,width=40)
destination.pack()

tk.Label(root,text="Broker Password (passed through not saved)").pack()
password= tk.Text(root,height=1,width=40).pack()

submit_button = tk.Button(root,text="SEND")
submit_button.pack()

root.mainloop()
