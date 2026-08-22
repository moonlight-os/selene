#!/usr/bin/env python3
"""Source-level contracts for the appliance panel's two host front ends."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PanelContractTest(unittest.TestCase):
    def source(self, path):
        return (ROOT / path).read_text(encoding="utf-8")

    def test_helper_replies_are_bounded(self):
        header = self.source("app/streaming/panel/helperclient.h")
        implementation = self.source("app/streaming/panel/helperclient.cpp")
        self.assertIn("k_MaxReplyBytes", header)
        self.assertIn("line.size() > k_MaxReplyBytes", implementation)

    def test_keyboard_and_pointer_hosts_offer_equivalent_navigation(self):
        window = self.source("app/streaming/panel/panelwindow.cpp")
        overlay = self.source("app/streaming/panel/panelmenu.cpp")
        for action in ("PageUp", "PageDown", "Home", "End"):
            self.assertIn("PanelModel::Key::" + action, window)
            self.assertIn("PanelModel::Key::" + action, overlay)
        self.assertIn("wheelEvent", window)
        self.assertIn("handleMouseWheel", overlay)

    def test_native_install_selection_crosses_the_helper_boundary(self):
        model = self.source("app/streaming/panel/panelmodel.cpp")
        self.assertIn('args["device"] = m_PendingInstallDevice', model)


if __name__ == "__main__":
    unittest.main(verbosity=2)
