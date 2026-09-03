#!/usr/bin/env python3

import platform
import sys
import ctypes

# Detect the correct dynamic library extension for Windows, macOS, or Linux
system_name = platform.system()
if system_name == "Windows":
    LZSS_SO_EXT = "dll"
elif system_name == "Darwin":
    LZSS_SO_EXT = "dylib"
else:
    LZSS_SO_EXT = "so"

LZSS_SO_FILE = f"./lzss.{LZSS_SO_EXT}"

if len(sys.argv) != 4:
    print ("Usage: lzss.py --[encode|decode] infile outfile")
    sys.exit()

lzss_functions = ctypes.CDLL(LZSS_SO_FILE)

mode   = sys.argv[1]
ifile  = sys.argv[2]
ofile  = sys.argv[3]

b_ifile = ifile.encode('utf-8')
b_ofile = ofile.encode('utf-8')

if mode == "--encode":
    lzss_functions.encode_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lzss_functions.encode_file(b_ifile, b_ofile)
elif mode == "--decode":
    lzss_functions.decode_file.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    lzss_functions.decode_file(b_ifile, b_ofile)
else:
    print ("Error, invalid mode parameter, use --encode or --decode")
