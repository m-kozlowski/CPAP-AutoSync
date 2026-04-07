try:
    Import("env")
except NameError:
    pass

import gzip
import os
import json


def fetch_timezone_data():
    """Download zones.json from GitHub and convert to a JS object literal.
    Returns the JS object string, or '{}' if the download fails.
    zones.json format: { "Region/City": "POSIX_STRING", ... }
    This is simpler and more robust than CSV (no regex, no quoting edge-cases).
    """
    try:
        import urllib.request
        url = "https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.json"
        print(f"  Fetching timezone data from {url}...")
        req = urllib.request.Request(url, headers={"User-Agent": "CPAP-AutoSync-Build/1.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))

        if not isinstance(data, dict) or len(data) == 0:
            print("  Warning: zones.json returned an empty or invalid object.")
            return "{}"

        # Re-serialize as compact JSON object (no extra whitespace)
        result = json.dumps(data, ensure_ascii=False, separators=(',', ':'))
        print(f"  Loaded {len(data)} timezone entries from zones.json.")
        return result
    except Exception as e:
        print(f"  Warning: Could not fetch timezone data ({e}). Embedded TZ will be empty.")
        return "{}"


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

    with open(input_file, 'r', encoding='utf-8') as f:
        html_content = f.read()

    # Embed timezone data at build time (replaces the placeholder in setup.html)
    tz_placeholder = "/*__TIMEZONE_DATA__*/{}"
    if tz_placeholder in html_content:
        tz_data = fetch_timezone_data()
        html_content = html_content.replace(tz_placeholder, tz_data)
        print(f"  Timezone data injected ({len(tz_data)} chars).")
    else:
        print("  Note: No timezone placeholder found in HTML; skipping TZ embedding.")

    html_bytes = html_content.encode('utf-8')
    compressed_content = gzip.compress(html_bytes)

    print(f"  HTML: {len(html_bytes)} bytes -> gzip: {len(compressed_content)} bytes ({100*len(compressed_content)//len(html_bytes)}%)")

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
