import math

import renderdoc as rd
import rdtest


class D3D11_Mesh_Zoo_PostVS(rdtest.TestCase):
    demos_test_name = 'D3D11_Mesh_Zoo'

    def check_capture(self):
        from renderdoc_mcp.mesh_decode import build_output_column_layout
        from renderdoc_mcp.rdutil import enum_name

        action = self.find_action("Quad")
        self.controller.SetFrameEvent(action.next.eventId, False)

        pipe: rd.PipeState = self.controller.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        if refl is None:
            raise rdtest.TestFailureException("Expected a bound vertex shader reflection")

        sig_params = []
        for sig in refl.outputSignature:
            sig_params.append({
                "name": sig.varName if sig.varName else sig.semanticIdxName,
                "semantic_name": sig.semanticName,
                "semantic_index": int(sig.semanticIndex),
                "system_value": enum_name(sig.systemValue),
                "var_type": enum_name(sig.varType),
                "comp_count": int(sig.compCount),
                "stream": int(sig.stream),
            })

        aligned = bool(pipe.HasAlignedPostVSData(rd.MeshDataStage.VSOut))
        columns = build_output_column_layout(sig_params, aligned)

        def find_column(**criteria):
            for c in columns:
                if all(c.get(k) == v for k, v in criteria.items()):
                    return c
            raise rdtest.TestFailureException(
                "Expected a column matching {} in {}".format(criteria, columns))

        pos_col = find_column(system_value="Position")
        color0_col = find_column(semantic_name="COLOR", semantic_index=0)
        color1_col = find_column(semantic_name="COLOR", semantic_index=1)

        if pos_col["byte_offset"] != 0:
            raise rdtest.TestFailureException(
                "Expected POSITION at byte 0, got {}".format(pos_col["byte_offset"]))
        if color0_col["byte_offset"] != 16:
            raise rdtest.TestFailureException(
                "Expected COLOR0 at byte 16, got {}".format(color0_col["byte_offset"]))

        expected_color1_offset = 32 if aligned else 24
        if color1_col["byte_offset"] != expected_color1_offset:
            raise rdtest.TestFailureException(
                "Expected COLOR1 at byte {} (aligned={}), got {}".format(
                    expected_color1_offset, aligned, color1_col["byte_offset"]))

        # Confirm GetPostVSData actually agrees this layout is real: the reported vertex stride
        # (sum of all column sizes) should not exceed the driver's own reported stride.
        mesh_fmt = self.controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        computed_stride = sum(c["comp_count"] * c["elem_byte_width"] for c in columns)
        if mesh_fmt.vertexResourceId == rd.ResourceId.Null():
            raise rdtest.TestFailureException("Expected valid post-VS data for the Quad draw")
        if computed_stride > mesh_fmt.vertexByteStride:
            raise rdtest.TestFailureException(
                "Computed stride {} exceeds driver-reported stride {}".format(
                    computed_stride, mesh_fmt.vertexByteStride))

        rdtest.log.success(
            "decode_post_vs_outputs column layout matches Mesh_Zoo's own reference offsets "
            "(POSITION=0, COLOR0=16, COLOR1={}, aligned={})".format(expected_color1_offset, aligned))

        self.check_decode_post_vs_outputs(action.next.eventId)

    def check_decode_post_vs_outputs(self, event_id):
        # End-to-end call of the actual orchestration function (not just the pure
        # build_output_column_layout helper checked above) against a real capture. This is the
        # regression check for the just-fixed Critical index-buffer bug: the Quad draw is
        # non-indexed (ctx->DrawInstanced(6, 2, 0, 0) in d3d11_mesh_zoo.cpp), so
        # _fetch_postvs_indices must return a plain sequential range and every row's "index" must
        # equal its "vertex_index".
        from renderdoc_mcp.mesh_decode import decode_post_vs_outputs

        self.controller.SetFrameEvent(event_id, False)

        result = decode_post_vs_outputs(
            self.controller, self.controller.GetStructuredFile(), event_id,
            stage="vsout", instance=0, view=0, preview_vertices=8, out_file=None)

        # decode_post_vs_outputs returns a plain dict, not the MCP R.ok()/R.err() envelope (that
        # only exists in server.py's tool wrapper) -- check for failure the same way server.py's
        # own decode_post_vs_outputs tool wrapper does.
        if result.get("error") or result.get("ok") is False:
            raise rdtest.TestFailureException(
                "decode_post_vs_outputs failed on the Quad draw: {}".format(result))

        vertex_count = result.get("vertex_count")
        if vertex_count != 6:
            raise rdtest.TestFailureException(
                "Expected 6 vertices per instance for the Quad draw (DrawInstanced(6, 2, 0, 0)), "
                "got {}".format(vertex_count))

        previews = result.get("vertex_previews", [])
        if len(previews) == 0:
            raise rdtest.TestFailureException("Expected at least one decoded vertex preview")

        for row in previews:
            if row.get("index") != row.get("vertex_index"):
                raise rdtest.TestFailureException(
                    "Non-indexed Quad draw should have index == vertex_index for every row, "
                    "got {}".format(row))

        pos_name = next(
            (s["name"] for s in result.get("semantics", []) if s.get("system_value") == "Position"),
            None)
        if pos_name is None:
            raise rdtest.TestFailureException("Expected a POSITION/SV_Position column in semantics")

        for row in previews:
            pos = row["values"].get(pos_name)
            clip = pos.get("clip") if isinstance(pos, dict) else None
            if not clip or len(clip) != 4 or not all(math.isfinite(v) for v in clip):
                raise rdtest.TestFailureException(
                    "Expected 4 finite clip-space floats for POSITION, got {}".format(clip))

            if result.get("unproject"):
                ndc = pos.get("ndc")
                if not ndc or len(ndc) != 3 or not all(math.isfinite(v) for v in ndc):
                    raise rdtest.TestFailureException(
                        "Expected 3 finite NDC floats for POSITION when unproject is set, "
                        "got {}".format(ndc))

        rdtest.log.success(
            "decode_post_vs_outputs decoded {} vertices end-to-end for the non-indexed Quad "
            "draw, with index == vertex_index for every row (regression check for the just-fixed "
            "index-buffer bug)".format(vertex_count))
