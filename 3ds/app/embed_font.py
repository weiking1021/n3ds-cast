import sys
from pathlib import Path
data=Path(sys.argv[1]).read_bytes()
Path(sys.argv[2]).write_text("static const unsigned char native_font[] __attribute__((aligned(4)))={"+",".join(map(str,data))+"};\n")
