#!/usr/bin/env python3
"""oDepth into a float24 (D24FS8) depth buffer on the FAST renderer (host render targets), 2026-11-11.

Why the waypoint / orb glows hide behind nearer geometry. Crackdown 2 copies the scene depth into a second depth
surface (EDRAM base 720, 1280x720) with a full-screen pass whose pixel shader writes oDepth = the resolved scene
depth texture, then draws the glows with GEQUAL against that copy. On the host-render-target path, float24 depth is
kept in a D32 buffer remapped from guest [0, 2) to host [0, 1): the viewport's depth range is halved
(draw_util "Remap the full [0...2) float24 range"). Rasterized depth gets that halving; a shader's oDepth did not
when depth_float24_convert_in_pixel_shader is off (the default): it went to gl_FragDepth as guest [0, 1]. So the
copy held TWICE the scene depth (measured: EDRAM snapshot of trace 4D5308BC_49176, base 720 == 2.000 x base 976 at
every pixel), every glow looked behind everything and failed GEQUAL.

Fix (Xenia Canary b15fcc7, "legacy path" in CompleteFragmentShader_DSV_DepthTo24Bit): when the shader writes
oDepth and the pixel shader modification is not a float24 conversion mode, store oDepth * 0.5 if the bound depth
buffer is D24FS8 (system flag kSysFlag_DepthFloat24). Unorm24 depth and the FSI path are unchanged.

    fw_odepth_float24.py <framework>
"""
import pathlib
import sys

sdk = pathlib.Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"
p = sdk / "src/graphics/pipeline/shader/spirv_translator_rb.cpp"
s = p.read_text(encoding="utf-8")
if "[cd] (fw_odepth_float24.py)" in s:
    print("fw_odepth_float24: already applied")
    sys.exit(0)
old = """      depth = SpirvShaderTranslator::Depth20e4To32(*builder_, depth_float24, 0, true, false,
                                                   ext_inst_glsl_std_450_);
      builder_->createStore(depth, output_fragment_depth_);
    }
  }
"""
new = """      depth = SpirvShaderTranslator::Depth20e4To32(*builder_, depth_float24, 0, true, false,
                                                   ext_inst_glsl_std_450_);
      builder_->createStore(depth, output_fragment_depth_);
    } else if (current_shader().writes_depth()) {
      // [cd] (fw_odepth_float24.py) guest oDepth [0, 1] -> host [0, 0.5] when the depth buffer is float24 (its
      // viewport depth range is halved for the [0, 2) remap), as in Canary b15fcc7.
      spv::Id depth = builder_->createLoad(output_fragment_depth_, spv::NoPrecision);
      spv::Id depth_is_float24 = builder_->createBinOp(
          spv::OpINotEqual, type_bool_,
          builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_system_constant_flags_,
                                builder_->makeUintConstant(kSysFlag_DepthFloat24)),
          const_uint_0_);
      spv::Id depth_halved = builder_->createNoContractionBinOp(spv::OpFMul, type_float_, depth,
                                                                builder_->makeFloatConstant(0.5f));
      builder_->createStore(
          builder_->createTriOp(spv::OpSelect, type_float_, depth_is_float24, depth_halved, depth),
          output_fragment_depth_);
    }
  }
"""
assert s.count(old) == 1, "fw_odepth_float24: anchor not found"
s = s.replace(old, new)
p.write_text(s, encoding="utf-8")
print("fw_odepth_float24: oDepth halved for float24 host depth")
