"""Disassemble a VA range of a PE: python disasm_range.py <hex VA> <hex len> [exe]"""
import sys, pefile, capstone
PATH=sys.argv[3] if len(sys.argv) > 3 else "/Users/heath/Virtua Tennis 3/dump/pc/VT3.exe"
pe = pefile.PE(PATH, fast_load=True)
data = open(PATH,"rb").read()
IB = pe.OPTIONAL_HEADER.ImageBase
def off(va):
    return pe.get_offset_from_rva(va - IB)
def dis(va, n):
    o = off(va)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for i in md.disasm(data[o:o+n], va):
        print(f"{i.address:08x}: {i.bytes.hex():24s} {i.mnemonic:8s} {i.op_str}")
if __name__ == "__main__":
    va = int(sys.argv[1],16); n = int(sys.argv[2],16)
    dis(va, n)
