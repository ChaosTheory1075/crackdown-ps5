#!/usr/bin/env python3
"""Crackdown 2 drawn in one tile (cd2_no_msaa): the game's baked tile-0 scissor snippets (2026-11-11).

With 2X MSAA the scene is drawn in two predicated tiles ({0,0,1280,484} and {0,480,1280,720}). Many per-object
command snippets in the game's data (same bytes in a stock and a patched Canary trace, untouched by every tile rect,
BeginTiling and viewport patch we tried; see Extras/CD2-NO-MSAA.md) carry a tile-0-only header:

    C0006100 00000000            SET_BIN_MASK_HI 0
    C0006000 00000003            SET_BIN_MASK_LO 3   (tile 0)
    C0032D01 00040080 0 0 01E40500  SET_CONSTANT PA_SC_WINDOW_OFFSET 0, SCISSOR_TL 0,0, SCISSOR_BR 1280,484

On the 360 the tile-1 pass skips it (mask 3 & select C = 0) and keeps the ring's tile-1 scissor (480-720). Drawn in one
tile (select 80000003) it runs and clips everything after it to rows 0-484: the floor below row 484 is missing.

cd_one_tile_scissor (default false; Crackdown 2 turns it on with cd2_no_msaa): when exactly that header (offset 0,
TL 0,0, BR 1280,484, bin mask 3) is written, BR becomes 1280,720, which is the union of the two tiles' scissors the
stock game ends up with. Nothing else changes.

    fw_one_tile_scissor.py <framework>     (any order after fw_tiling_log.py)
"""
import pathlib
import sys

MARK = "[cd] (fw_one_tile_scissor.py)"


def main():
    fw = pathlib.Path(sys.argv[1])
    root = fw / "glue" / "rexglue-sdk-main" if (fw / "glue").exists() else fw
    cp = root / "src" / "graphics" / "command_processor.cpp"
    t = cp.read_text(encoding="utf-8")
    if MARK in t:
        print("fw_one_tile_scissor: command_processor.cpp already applied")
    else:
        patch_cp(cp, t)
    patch_vk(root)


def patch_cp(cp, t):
    anchor = "void CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,\n"
    if t.count(anchor) != 1:
        sys.exit("fw_one_tile_scissor: WriteRegistersFromMem not found once")
    helper = (
        "REXCVAR_DEFINE_BOOL(cd_one_tile_scissor, false, \"GPU\",  // " + MARK + "\n"
        "                    \"Crackdown 2 in one tile: a baked tile-0 scissor (1280x484) covers the whole 1280x720\");\n"
        "static inline void CdOneTileScissor(uint32_t* v, uint64_t bin_mask, uint32_t start, uint32_t n) {  // " + MARK + "\n"
        "  if (start > 0x2082 || start + n <= 0x2082 || bin_mask != 3 || !REXCVAR_GET(cd_one_tile_scissor)) return;\n"
        "  if (v[0x2080] == 0 && v[0x2081] == 0 && v[0x2082] == 0x01E40500) v[0x2082] = 0x02D00500;\n"
        "}\n\n")
    t = t.replace(anchor, helper + anchor)
    # the fast path's copy (fw_draw_worker.py may add a line after it)
    fast = "    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);\n"
    i = t.index(anchor)
    j = t.find(fast, i)
    if j < 0 or j - i > 600:
        sys.exit("fw_one_tile_scissor: fast path not found in WriteRegistersFromMem")
    j += len(fast)
    t = t[:j] + "    CdOneTileScissor(register_file_->values, bin_mask_, start_index, num_registers);  // " + MARK + "\n" + t[j:]
    slow = ("  for (uint32_t i = 0; i < num_registers; ++i) {\n"
            "    uint32_t data = memory::load_and_swap<uint32_t>(base + i);\n"
            "    WriteRegister(start_index + i, data);\n"
            "  }\n")
    if t.count(slow) != 1:
        sys.exit("fw_one_tile_scissor: slow path not found once")
    t = t.replace(slow, slow + "  CdOneTileScissor(register_file_->values, bin_mask_, start_index, num_registers);  // " + MARK + "\n")
    # (Not done: also running tile-1-only draws (mask C). Tried 2026-11-11: no change on the frame with the game's
    # tile-count copies at 1 (md5 identical), and with them at 2 it drew the tile-1 copies of full-screen passes
    # over the top tile. The game's own count copies at 1 (cd2_features.cpp) are the fix for those.)
    cp.write_text(t, encoding="utf-8")
    print("patched src/graphics/command_processor.cpp (cd one-tile scissor)")


def patch_vk(root):

    # The shaders' compiled-in literal constants (HLSL static consts, loaded with LOAD_ALU_CONSTANT from the shader
    # data on disc: 473 copies of 1/484 in the ISO, e.g. (1/1280, 1/484, 0.2, 0) at 0x54E25F84) are the texel size of
    # the per-tile screen buffers (1280x484). Shaders scale the tile-relative pixel position by them to read e.g. the
    # screen normals copy (752E87436D45FF64, DA42ED9C4344717E: vPos * c255.xy -> tf15). In one tile the buffer is
    # 1280x720 and the pixel position runs 0..720, so 1/484 becomes 1/720 (the exact one-tile equivalent).
    vk = root / "src" / "graphics" / "vulkan" / "command_processor.cpp"
    v = vk.read_text(encoding="utf-8")
    if MARK not in v:
        head = ("  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&\n"
                "      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {\n")
        if v.count(head) != 1:
            sys.exit("fw_one_tile_scissor: shader constant branch not found once")
        i = v.index(head)
        anchor = "    if (frame_open_"  # (fw_draw_worker.py makes it "if (frame_open_ && !sr_worker_on_)")
        j = v.index(anchor, i)
        v = v[:j] + (
            "    if (REXCVAR_GET(cd_one_tile_scissor)) {  // " + MARK + " baked 1/484 -> 1/720\n"
            "      for (uint32_t r = start_index; r <= end_index; ++r)\n"
            "        if (register_file_->values[r] == 0x3B0767ABu) register_file_->values[r] = 0x3AB60B61u;\n"
            "      // The point-light pass (VS 70BA994BA133FD9F, PS 2759142D58DB627E) maps clip-space y to the screen\n"
            "      // normals copy with c35 = (0, -s, 0, s + 0.4453/H): stock 1280x484 copy -> s = 360/484\n"
            "      // (BF3E69C9, w 3F3EA615); in one tile the game computes s = 0.74583 (BF3EEEEF, w 3F3F2B0C) for a\n"
            "      // copy that is 720 high. Same pair -> s = 360/720, w = 360.4453/720 (replay: the hard-edged floor\n"
            "      // rectangles below the old tile boundary go away; Extras/CD2-NO-MSAA.md).\n"
            "      for (uint32_t r = start_index; r + 2 <= end_index; ++r)\n"
            "        if (register_file_->values[r] == 0xBF3EEEEFu && register_file_->values[r + 2] == 0x3F3F2B0Cu) {\n"
            "          register_file_->values[r] = 0xBF000000u;\n"
            "          register_file_->values[r + 2] = 0x3F002888u;\n"
            "        }\n"
            "    }\n") + v[j:]
        k = v.index("namespace rex::graphics")  # the first namespace line, after the includes
        v = v[:k] + ("namespace rex::graphics {\nREXCVAR_DECLARE(bool, cd_one_tile_scissor);  // " + MARK +
                     " defined in command_processor.cpp\n}  // namespace rex::graphics\n\n") + v[k:]
        vk.write_text(v, encoding="utf-8")
        print("patched src/graphics/vulkan/command_processor.cpp (cd one-tile 1/484 literals)")


if __name__ == "__main__":
    main()
