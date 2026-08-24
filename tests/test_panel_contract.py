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

    def test_native_control_centre_exposes_guarded_os_updates(self):
        model = self.source("app/streaming/panel/panelmodel.cpp")
        header = self.source("app/streaming/panel/panelmodel.h")
        self.assertIn('result.value("update_available")', model)
        self.assertIn('QStringLiteral("Update Moonlight OS")', model)
        self.assertIn('QStringLiteral("update")', model)
        self.assertIn("m_UpdateAvailable", header)

    def test_native_control_centre_toggles_beta_updates(self):
        model = self.source("app/streaming/panel/panelmodel.cpp")
        header = self.source("app/streaming/panel/panelmodel.h")
        self.assertIn('QStringLiteral("Beta updates\\t%1")', model)
        self.assertIn('QStringLiteral("update_channel")', model)
        self.assertIn('result.value("update_channel")', model)
        self.assertIn("QString updateChannel", header)

    def test_welcome_keyboard_configuration_returns_to_welcome(self):
        model = self.source("app/streaming/panel/panelmodel.cpp")
        header = self.source("app/streaming/panel/panelmodel.h")
        self.assertIn('"Configure keyboard & time zone"', model)
        self.assertIn("m_RegionReturnsToWelcome", header)
        self.assertIn('action == "Done"', model)
        self.assertIn("m_RegionReturnsToWelcome ? Screen::Welcome", model)

    def test_tailscale_login_qr_crosses_model_and_painter(self):
        model = self.source("app/streaming/panel/panelmodel.cpp")
        header = self.source("app/streaming/panel/panelmodel.h")
        painter = self.source("app/streaming/panel/panelpainter.cpp")
        painter_header = self.source("app/streaming/panel/panelpainter.h")
        self.assertIn('result.value("login_qr")', model)
        self.assertIn("m_TailscaleLoginQr", header)
        self.assertIn("out.qrCode = m_TailscaleLoginQr", model)
        self.assertIn("QImage qrCode", painter_header)
        self.assertIn("Qt::FastTransformation", painter)
        self.assertIn("painter.drawImage", painter)

    def test_first_run_prefers_active_ethernet_over_wifi_setup(self):
        model = self.source("app/streaming/panel/panelmodel.cpp")
        header = self.source("app/streaming/panel/panelmodel.h")
        self.assertIn("QString m_ConnectionType", header)
        self.assertIn('m_ConnectionType == QLatin1String("ethernet")', model)
        self.assertIn('"Continue with Ethernet", "Use Wi-Fi instead", "Back"', model)
        self.assertIn('action == "Continue with Ethernet"', model)
        self.assertIn('result.value("connection_type")', model)


if __name__ == "__main__":
    unittest.main(verbosity=2)
