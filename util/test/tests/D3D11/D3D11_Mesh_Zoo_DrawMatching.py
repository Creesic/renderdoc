import rdtest


class D3D11_Mesh_Zoo_DrawMatching(rdtest.TestCase):
    demos_test_name = 'D3D11_Mesh_Zoo'

    def check_capture(self):
        from renderdoc_mcp.draw_matching import find_corresponding_draws

        # d3d11_mesh_zoo.cpp draw order (see util/test/demos/d3d11/d3d11_mesh_zoo.cpp):
        #   ctx->Draw(3, 10);                    // unmarked, non-indexed TRIANGLELIST -- 'previous'
        #   setMarker("Quad");
        #   ctx->DrawInstanced(6, 2, 0, 0);       // the "Quad" draw -- 'next' of the Quad marker
        #   setMarker("Points");
        #   ctx->IASetPrimitiveTopology(POINTLIST);
        #   ctx->Draw(4, 6);                     // the "Points" draw -- 'next' of the Points marker
        #
        # The unmarked draw right before "Quad" is a same-kind candidate (also a non-indexed
        # TRIANGLELIST draw with identical VB/RT/DS bindings); the "Points" draw is a
        # different-kind candidate (POINTLIST topology).
        quad_marker = self.find_action("Quad")
        points_marker = self.find_action("Points")

        ref_action = quad_marker.next
        same_kind_action = quad_marker.previous
        diff_kind_action = points_marker.next

        ref_eid = ref_action.eventId
        same_kind_eid = same_kind_action.eventId
        diff_kind_eid = diff_kind_action.eventId

        self.controller.SetFrameEvent(ref_eid, False)
        structured_file = self.controller.GetStructuredFile()

        # Regression check for the fix in 238963961: PipeState.GetTopology() (nonexistent, always
        # swallowed to None by serialize.py's _try()) was replaced with GetPrimitiveTopology(), and
        # the always-true "if ib is not None" check on GetIBuffer() (which never returns Python
        # None) was replaced with a real ResourceId check. Both feed draw_shape_key()'s prefilter
        # tuple. Before the fix, topology was None and has_index_buffer was always True for every
        # draw, so the shape prefilter could never distinguish a TRIANGLELIST draw from a POINTLIST
        # draw -- on this exact capture, that let the "Points" draw (different topology) outrank
        # the genuine same-kind triangle draw, with *higher* confidence (0.933 vs 0.900) and a
        # *higher* geometry sub-signal (0.833 vs 0.750), because raw element-count closeness
        # (6 vs 4 for Points, 6 vs 3 for the same-kind draw) happens to favor Points on this
        # fixture, with nothing else to disambiguate them.
        #
        # After the fix, topology and has_index_buffer are correct, so draw_shape_key() differs
        # between the (TriangleList, ..., no-index-buffer, ...) reference and the Points draw's
        # (PointList, ...) key -- the shape prefilter now excludes the Points draw entirely before
        # scoring, rather than merely ranking it below the same-kind draw.
        result = find_corresponding_draws(
            self.controller, structured_file, ref_eid,
            self.controller, structured_file,
            event_ids_b=[same_kind_eid, diff_kind_eid],
        )

        if result.get("ok") is False:
            raise rdtest.TestFailureException(
                "find_corresponding_draws failed unexpectedly: {}".format(result))

        if not result.get("shape_prefilter_applied"):
            raise rdtest.TestFailureException(
                "Expected the shape prefilter to fire (TriangleList reference vs. a mixed "
                "TriangleList/PointList candidate pool), got {}".format(result))

        if result.get("prefiltered_count") != 1:
            raise rdtest.TestFailureException(
                "Expected exactly 1 candidate to survive the shape prefilter (the same-kind "
                "TriangleList draw), got prefiltered_count={} in {}".format(
                    result.get("prefiltered_count"), result))

        candidates = result.get("candidates", [])
        candidate_ids = [c["event_id"] for c in candidates]

        if diff_kind_eid in candidate_ids:
            raise rdtest.TestFailureException(
                "The different-kind (Points/PointList) draw (eid {}) should have been excluded "
                "by the shape prefilter, but it appeared in candidates: {}".format(
                    diff_kind_eid, result))

        if candidate_ids != [same_kind_eid]:
            raise rdtest.TestFailureException(
                "Expected candidates to contain exactly the same-kind draw (eid {}), got {}".format(
                    same_kind_eid, result))

        same_kind_candidate = candidates[0]
        if not (0.5 < same_kind_candidate["confidence"] <= 1.0):
            raise rdtest.TestFailureException(
                "Expected a meaningfully positive confidence for the genuine same-kind match, "
                "got {}".format(same_kind_candidate))
        if not (0.5 < same_kind_candidate["signals"]["geometry"] <= 1.0):
            raise rdtest.TestFailureException(
                "Expected a meaningfully positive geometry signal for the genuine same-kind "
                "match, got {}".format(same_kind_candidate))

        rdtest.log.success(
            "find_corresponding_draws correctly excludes the different-kind PointList draw (eid "
            "{}) from the shape-prefiltered candidate pool and ranks the genuine same-kind "
            "TriangleList draw (eid {}) as the sole match, confidence={:.3f} geometry={:.3f} "
            "(regression check for the GetTopology()/GetIBuffer() fix in 238963961)".format(
                diff_kind_eid, same_kind_eid, same_kind_candidate["confidence"],
                same_kind_candidate["signals"]["geometry"]))
