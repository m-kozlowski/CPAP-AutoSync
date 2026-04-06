try:
    Import("env")
except NameError:
    pass

import gzip
import os

def generate_header(*args, **kwargs):
    print("Generating compressed HTML payload...")
    input_file = "src/web/setup.html"
    output_file = "include/setup_html_gz.h"
    
    if not os.path.exists(input_file):
        print(f"Warning: {input_file} not found. Creating empty file to prevent build errors.")
        os.makedirs(os.path.dirname(output_file), exist_ok=True)
        with open(output_file, 'w') as f:
            f.write("#pragma once\nconst uint8_t setup_html_gz[] = {0};\nconst size_t setup_html_gz_len = 0;\n")
        return
        
    with open(input_file, 'rb') as f:
        html_content = f.read()
        
    compressed_content = gzip.compress(html_content)
    
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    with open(output_file, 'w') as f:
        f.write("#pragma once\n\n")
        f.write(f"// Generated from {input_file}\n")
        f.write("const uint8_t setup_html_gz[] = {\n")
        
        hex_array = [f"0x{b:02x}" for b in compressed_content]
        for i in range(0, len(hex_array), 16):
            f.write("    " + ", ".join(hex_array[i:i+16]) + ",\n")
            
        f.write("};\n\n")
        f.write(f"const size_t setup_html_gz_len = {len(compressed_content)};\n")

generate_header()
