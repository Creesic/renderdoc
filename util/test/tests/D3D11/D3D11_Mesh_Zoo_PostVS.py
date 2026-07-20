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
