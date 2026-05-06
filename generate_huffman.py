import sys

file_path = "/Users/ethan/.gemini/antigravity/brain/1a095ddf-209e-4ed5-a5bc-ccf71dfdbbfe/.system_generated/steps/115/content.md"

try:
    with open(file_path, "r") as f:
        content = f.read()

    lines = content.split('\n')
    table_lines = []
    in_table = False
    for line in lines:
        if "const nghttp2_huff_sym huff_sym_table[]" in line:
            in_table = True
            continue
        if in_table:
            if "};" in line:
                break
            table_lines.append(line)

    with open("/Users/ethan/Downloads/uproxy/include/uproxy/huffman_table.h", "w") as f:
        f.write("#pragma once\n\n#include <cstdint>\n\nnamespace uproxy {\n\n")
        f.write("struct HpackHuffmanSym { uint32_t nbits; uint32_t code; };\n\n")
        f.write("constexpr HpackHuffmanSym HPACK_HUFFMAN_SYM_TABLE[257] = {\n")
        for line in table_lines:
            line = line.replace("{", "{ ").replace("}", " }").replace("u", "")
            f.write(line + "\n")
        f.write("};\n\n} // namespace uproxy\n")
    print("Huffman table generated.")
except Exception as e:
    print("Error:", e)
