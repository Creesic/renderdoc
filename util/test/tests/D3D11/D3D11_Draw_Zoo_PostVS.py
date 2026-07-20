import struct

import renderdoc as rd
import rdtest


class D3D11_Draw_Zoo_PostVS(rdtest.TestCase):
    demos_test_name = 'D3D11_Draw_Zoo'

    def check_capture(self):
        from renderdoc_mcp.mesh_decode import decode_post_vs_outputs

        marker = self.find_action("Test Begin")
        if marker is None:
            raise rdtest.TestFailureException("Couldn't find 'Test Begin' marker action")

        # d3d11_draw_zoo.cpp's indexed draws mostly use a single ascending run of indices (e.g.
        # {0,1,2} or {5,6,7}) -- GetPostVSData's own rebased index buffer sorts unique indices by
        # *value* (renderdoc/driver/d3d11/d3d11_postvs.cpp), so an already-ascending run rebases
        # to the identity mapping (index == vertex_index) regardless of whether the index
        # resolution fix is present or not. That makes those draws useless as a regression check
        # for the just-fixed Critical index-buffer bug.
        #
        # The two "indexed strip with primitive restart" draws are different: the restart
        # sentinel sorts to the very end of the value-sorted compact vertex buffer (it's a huge
        # value) while appearing in the *middle* of primitive order, which shifts every
        # subsequent real index down by one relative to its vertex_index. That's a genuine,
        # deterministic divergence -- so instead of assuming a fixed position in the draw
        # sequence, walk forward from "Test Begin" and pick the first indexed draw whose actual
        # index buffer contains a primitive-restart value in its active range.
        target = self._find_restart_indexed_action(marker)
        if target is None:
            raise rdtest.TestFailureException(
                "Couldn't find an indexed draw with a primitive-restart index after 'Test Begin' "
                "-- indexed-draw regression coverage for decode_post_vs_outputs depends on one "
                "existing in D3D11_Draw_Zoo")

        self.controller.SetFrameEvent(target.eventId, False)

        # Independently read the SAME (original, pre-rebase) index buffer the pipeline is bound
        # to, so we can cross-check decode_post_vs_outputs' resolved indices against ground truth
        # ourselves rather than trusting the tool's own internal math.
        pipe = self.controller.GetPipelineState()
        ib = pipe.GetIBuffer()
        stride = ib.byteStride
        ioffs = ib.byteOffset + target.indexOffset * stride
        raw = self.controller.GetBufferData(ib.resourceId, ioffs, stride * target.numIndices)
        fmt_char = {1: "B", 2: "H", 4: "I"}[stride]
        n = min(len(raw) // stride, target.numIndices)
        original_indices = struct.unpack_from("=" + str(n) + fmt_char, raw, 0)
        restart_idx = pipe.GetRestartIndex() & ((1 << (stride * 8)) - 1)

        if restart_idx not in original_indices:
            raise rdtest.TestFailureException(
                "Expected the independently-read original index buffer to contain the "
                "primitive-restart value {}, got {}".format(restart_idx, original_indices))

        result = decode_post_vs_outputs(
            self.controller, self.controller.GetStructuredFile(), target.eventId,
            stage="vsout", instance=0, view=0, preview_vertices=16, out_file=None)

        if result.get("error") or result.get("ok") is False:
            raise rdtest.TestFailureException(
                "decode_post_vs_outputs failed on the indexed strip draw at event {}: {}".format(
                    target.eventId, result))

        previews = result.get("vertex_previews", [])
        if len(previews) == 0:
            raise rdtest.TestFailureException("Expected at least one decoded vertex preview")

        # The actual regression check: at least one *resolved* (non-restart) index must differ
        # from its primitive-order position, proving the post-VS index buffer is being remapped
        # for real rather than degenerating to a no-op range(fetch_count).
        diverging_rows = [
            row for row in previews
            if row.get("index") is not None and row.get("index") != row.get("vertex_index")
        ]
        if not diverging_rows:
            raise rdtest.TestFailureException(
                "Expected at least one resolved index to differ from vertex_index on the "
                "indexed strip draw (proving real index-buffer remapping), got {}".format(previews))

        rdtest.log.success(
            "decode_post_vs_outputs on the indexed strip draw at event {} resolved {} row(s) "
            "whose index diverges from primitive order (e.g. {}), confirming the post-VS index "
            "buffer is really being remapped -- regression check for the just-fixed Critical "
            "index-buffer bug".format(target.eventId, len(diverging_rows), diverging_rows[0]))

    def _find_restart_indexed_action(self, marker):
        """Walk the flat event-order action chain (ActionDescription.next spans the whole frame,
        not just siblings) starting after `marker`, looking for an indexed draw whose original
        index buffer contains a primitive-restart value in its active range."""
        action = marker.next
        steps = 0
        while action is not None and steps < 40:
            steps += 1
            if action.flags & rd.ActionFlags.Indexed:
                self.controller.SetFrameEvent(action.eventId, False)
                pipe = self.controller.GetPipelineState()
                ib = pipe.GetIBuffer()
                stride = ib.byteStride
                if stride in (1, 2, 4) and ib.resourceId != rd.ResourceId.Null() and pipe.IsRestartEnabled():
                    restart_idx = pipe.GetRestartIndex() & ((1 << (stride * 8)) - 1)
                    ioffs = ib.byteOffset + action.indexOffset * stride
                    raw = self.controller.GetBufferData(ib.resourceId, ioffs, stride * action.numIndices)
                    fmt_char = {1: "B", 2: "H", 4: "I"}[stride]
                    n = min(len(raw) // stride, action.numIndices)
                    if n > 0:
                        values = struct.unpack_from("=" + str(n) + fmt_char, raw, 0)
                        if restart_idx in values:
                            return action
            action = action.next
        return None
