#!/usr/bin/env python3
"""Generate standard Linux Flattened Device Tree binary (shamrock.dtb)
for General Mobile GM 5 Plus (MSM8952).

This DTB is appended or embedded into the Android boot.img so that LK (LittleKernel)
identifies the board (msm-id 264 / board-id) and boots the shim with correct memory tags.
"""
import os, sys
import pyfdt.pyfdt as fdt

def build_dtb(out_path):
    tree = fdt.Fdt()
    root = fdt.FdtNode("/")
    
    root.add_subnode(fdt.FdtPropertyStrings("model", ["General Mobile GM 5 Plus"]))
    root.add_subnode(fdt.FdtPropertyStrings("compatible", ["generalmobile,shamrock", "qcom,msm8952"]))
    root.add_subnode(fdt.FdtPropertyWords("qcom,msm-id", [
        264, 0x0,
        264, 0x10000,
        264, 0x20000,
        264, 0x30000,
    ]))
    root.add_subnode(fdt.FdtPropertyWords("qcom,board-id", [
        0x1000b, 0,
        11, 0,
        0x10008, 0,
        8, 0,
        0, 0,
    ]))
    root.add_subnode(fdt.FdtPropertyWords("#address-cells", [2]))
    root.add_subnode(fdt.FdtPropertyWords("#size-cells", [2]))

    # /chosen
    chosen = fdt.FdtNode("chosen")
    chosen.add_subnode(fdt.FdtPropertyStrings("bootargs", ["console=ttyHSL0,115200,n8 androidboot.hardware=qcom"]))
    root.add_subnode(chosen)

    # /memory
    memory = fdt.FdtNode("memory")
    memory.add_subnode(fdt.FdtPropertyStrings("device_type", ["memory"]))
    # 0x0 0x80000000 0x0 0xc0000000 (3GB RAM)
    memory.add_subnode(fdt.FdtPropertyWords("reg", [0x0, 0x80000000, 0x0, 0xc0000000]))
    root.add_subnode(memory)

    # /reserved-memory
    res_mem = fdt.FdtNode("reserved-memory")
    res_mem.add_subnode(fdt.FdtPropertyWords("#address-cells", [2]))
    res_mem.add_subnode(fdt.FdtPropertyWords("#size-cells", [2]))
    res_mem.add_subnode(fdt.FdtProperty("ranges"))

    # Framebuffer carve-out: 0x83000000 size 0x1400000 (FB at 0x83200000)
    fb_node = fdt.FdtNode("splash_region@83000000")
    fb_node.add_subnode(fdt.FdtPropertyWords("reg", [0x0, 0x83000000, 0x0, 0x01400000]))
    fb_node.add_subnode(fdt.FdtPropertyStrings("label", ["cont_splash_mem"]))
    res_mem.add_subnode(fb_node)

    # Ramoops / persistent log carve-out: 0xdffc0000 size 0x40000
    ramoops_node = fdt.FdtNode("ramoops_region@dffc0000")
    ramoops_node.add_subnode(fdt.FdtPropertyWords("reg", [0x0, 0xdffc0000, 0x0, 0x00040000]))
    ramoops_node.add_subnode(fdt.FdtPropertyStrings("label", ["ramoops_mem"]))
    res_mem.add_subnode(ramoops_node)

    root.add_subnode(res_mem)
    tree.add_rootnode(root)

    dtb_bytes = tree.to_dtb()
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(dtb_bytes)
    print(f"Successfully generated {out_path} ({len(dtb_bytes)} bytes)")

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "/home/atakan/GM5PlusPort/out/shamrock.dtb"
    build_dtb(out)
