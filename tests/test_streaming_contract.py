#!/usr/bin/env python3
"""Source-level safety contracts for Moonlight OS streaming integration."""

from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StreamingContractTest(unittest.TestCase):
    def source(self, path):
        return (ROOT / path).read_text(encoding="utf-8")

    def test_quic_failure_does_not_run_legacy_port_probe(self):
        session = self.source("app/streaming/session.cpp")
        quic_guard = session.index("if (s_ActiveSession->m_QuicTransport != nullptr)")
        legacy_probe = session.index("LiTestClientConnectivity", quic_guard)
        self.assertLess(quic_guard, legacy_probe)
        self.assertIn("m_PortTestResults = 0", session[quic_guard:legacy_probe])

    def test_quic_shutdown_is_single_owner_and_mutex_protected(self):
        transport = self.source("app/streaming/quictransport.cpp")
        self.assertIn("std::mutex stateMutex", transport)
        self.assertIn("bool shutdownRequested = false", transport)
        self.assertIn("void shutdownConnection(", transport)
        self.assertIn(
            "connection != nullptr && !shutdownRequested && !connectionFinished",
            transport,
        )
        self.assertIn("HQUIC connectionToClose = nullptr", transport)
        self.assertIn("connectionToClose = connection", transport)
        self.assertIn("connection = nullptr", transport)
        self.assertEqual(transport.count("ConnectionClose("), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
