import renderdoc as rd
import rdtest


class D3D11_Discard_View_Provenance(rdtest.TestCase):
    demos_test_name = 'D3D11_Discard_View'

    def find_copy_actions(self, actions):
        found = []
        for a in actions:
            if a.flags & rd.ActionFlags.Copy:
                found.append(a)
            found += self.find_copy_actions(a.children)
        return found

    def check_capture(self):
        from renderdoc_mcp.analysis import classify_producer_action, copy_hop_is_safe, find_texture_description, mip_dims
        from renderdoc_mcp.session import expand_action_flags

        copy_actions = self.find_copy_actions(self.controller.GetRootActions())

        if len(copy_actions) != 1:
            raise rdtest.TestFailureException(
                "Expected exactly one Copy action, found {}".format(len(copy_actions)))

        action = copy_actions[0]

        last_action = self.get_last_action()
        if not (last_action.flags & rd.ActionFlags.Present) or last_action.eventId <= action.eventId:
            raise rdtest.TestFailureException(
                "Expected the Copy action (EID {}) to be immediately followed by the frame's "
                "final Present (EID {})".format(action.eventId, last_action.eventId))

        # (a) classify_producer_action must route this action to the copy-hop-crossing logic,
        # the same way trace_pixel_provenance does via expand_action_flags().
        flags_names = expand_action_flags(rd, int(action.flags))
        kind = classify_producer_action(flags_names)
        if kind != "copy":
            raise rdtest.TestFailureException(
                "Expected Copy action to classify as 'copy', got '{}'".format(kind))

        # (b) copySource must be populated -- this is what the copy-hop crossing reads.
        if action.copySource == rd.ResourceId.Null():
            raise rdtest.TestFailureException("Copy action has no copySource")

        # (c) copy_hop_is_safe must consider this full-surface, equally-sized copy safe, exactly
        # as trace_pixel_provenance's dimension check does before crossing the hop.
        dest_tex = find_texture_description(self.controller, action.copyDestination)
        src_tex = find_texture_description(self.controller, action.copySource)

        if dest_tex is None or src_tex is None:
            raise rdtest.TestFailureException(
                "Could not resolve texture description for copy source/destination")

        dest_dims = mip_dims(int(dest_tex.width), int(dest_tex.height), 0)
        source_dims = mip_dims(int(src_tex.width), int(src_tex.height), 0)

        if not copy_hop_is_safe(dest_dims, source_dims):
            raise rdtest.TestFailureException(
                "Expected copy_hop_is_safe(dest={}, source={}) to be True for this full-surface "
                "copy".format(dest_dims, source_dims))

        # (d) the actual proof the crossing is meaningful: at the point in time
        # trace_pixel_provenance would cross into copySource (i.e. at the copy action's own
        # event, before it executes), there must be a real producer draw already recorded in
        # the source texture's pixel history for the tool to find.
        self.controller.SetFrameEvent(action.eventId, True)

        x = src_tex.width // 2
        y = src_tex.height // 2

        hist = self.controller.PixelHistory(
            action.copySource, x, y, rd.Subresource(), src_tex.format.compType)

        if len(hist) == 0:
            raise rdtest.TestFailureException(
                "Expected at least one pixel history entry on the copy source at ({}, {}) "
                "before the copy".format(x, y))

        rdtest.log.success(
            "Copy/Resolve hop crossing validated: Copy action EID {} classifies as 'copy', "
            "copy_hop_is_safe() agrees dest/source dims match, and its copySource has {} pixel "
            "history entries before the copy".format(action.eventId, len(hist)))
