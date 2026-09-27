"""Download a Piper voice (.onnx + .onnx.json) from Hugging Face at build time.

Voice names look like  <lang>_<REGION>-<speaker>-<quality>,  e.g. en_US-lessac-medium.
"""

import sys
import urllib.request
from pathlib import Path

name = sys.argv[1]
lang_region, rest = name.split("-", 1)
speaker, quality = rest.rsplit("-", 1)
lang = lang_region.split("_")[0]
base = f"https://huggingface.co/rhasspy/piper-voices/resolve/main/{lang}/{lang_region}/{speaker}/{quality}/{name}"

out = Path("/voices")
out.mkdir(exist_ok=True)
for suffix in (".onnx", ".onnx.json"):
    print(f"downloading {base}{suffix}")
    urllib.request.urlretrieve(base + suffix, out / f"{name}{suffix}")
