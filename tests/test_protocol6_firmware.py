import unittest
from pathlib import Path


MAIN = Path(__file__).parents[1] / "firmware" / "tiny_touch_unified" / "main"


class ProtocolSixFirmwareTests(unittest.TestCase):
    def source(self, name: str) -> str:
        return (MAIN / name).read_text()

    def test_no_firmware_software_restart_path(self) -> None:
        source = "\n".join(path.read_text() for path in MAIN.glob("*.c"))
        self.assertNotIn("esp_restart", source)
        self.assertNotIn("watchdog_reboot", source)
        self.assertNotIn("reset_usb_boot", source)

    def test_protocol_six_has_one_stable_usb_descriptor(self) -> None:
        cmake = (MAIN.parent / "CMakeLists.txt").read_text()
        descriptors = self.source("usb_descriptors.c")
        self.assertIn("TINYTOUCH_PROTOCOL_VERSION=6", cmake)
        self.assertIn("return tiny_touch_configuration_descriptor;", descriptors)
        self.assertNotIn("tiny_touch_hid_configuration_descriptor", descriptors)
        self.assertNotIn("tiny_touch_piv_configuration_descriptor", descriptors)

    def test_usb_resume_reenumerates_without_restarting_firmware(self) -> None:
        usb = self.source("usb_ccid.c")
        self.assertIn("void tud_resume_cb(void)", usb)
        self.assertIn("resume_reconnect_task", usb)
        self.assertIn("tud_disconnect();", usb)
        self.assertIn("tud_connect();", usb)

    def test_persistence_swaps_one_live_config_blob(self) -> None:
        source = self.source("device_config.c")
        self.assertIn("storage_write(STORAGE_CONFIG", source)
        self.assertIn("replace_locked", source)
        self.assertIn("device_config_factory_reset", source)
        self.assertNotIn('"hid_key"', source)
        self.assertNotIn('"hid_hosts"', source)
        self.assertNotIn("mode == DEVICE_MODE_HID && value->hid_host_count == 0", source)

    def test_host_listing_is_read_only_and_uses_lowercase_ids(self) -> None:
        console = self.source("config_console.c")
        self.assertIn('strcmp(command, "HOST LIST")', console)
        self.assertIn('"0123456789abcdef"', console)
        self.assertIn('"OK HOST LIST ids=%s capacity=%u"', console)

    def test_ota_stages_without_changing_the_current_runtime(self) -> None:
        console = self.source("config_console.c")
        update = self.source("firmware_update.c")
        main = self.source("main.c")
        self.assertIn("OK OTA STAGED power_cycle=required", console)
        self.assertIn("stage_update_marker", update)
        self.assertIn("firmware_update_staged", update)
        # The staged image is installed by the next boot, never by the running session.
        self.assertIn("firmware_update_apply_pending();", main)
        self.assertNotIn("install_staged(", console)
        self.assertIn('strcmp(command, "OTA ABORT") == 0', console)
        self.assertNotIn("fingerprint_prepare_for_restart", console)

    def test_piv_create_is_live_and_status_reports_readiness(self) -> None:
        console = self.source("config_console.c")
        piv = self.source("piv.c")
        self.assertIn('strcmp(command, "PIV CREATE")', console)
        self.assertIn('piv_uses_provisioned_keys() ? "ready" : "unconfigured"', console)
        self.assertIn("piv_create_identity", piv)
        self.assertIn("piv_reload_keys()", piv)

    def test_piv_certificates_separate_login_and_keychain_usage(self) -> None:
        piv = self.source("piv.c")
        self.assertIn("#define PIV_IDENTITY_SCHEMA 3", piv)
        self.assertIn("MBEDTLS_X509_KU_KEY_ENCIPHERMENT", piv)
        self.assertIn("MBEDTLS_X509_KU_DIGITAL_SIGNATURE", piv)
        self.assertIn("if (result == 0 && !key_management)", piv)
        self.assertNotIn(
            "MBEDTLS_X509_KU_DIGITAL_SIGNATURE | "
            "MBEDTLS_X509_KU_KEY_ENCIPHERMENT",
            piv,
        )

    def test_piv_token_identifier_changes_with_the_identity(self) -> None:
        piv = self.source("piv.c")
        console = self.source("config_console.c")
        self.assertIn("set_chuid_guid(cert_9a_der, cert_9a_der_len)", piv)
        self.assertIn("set_chuid_guid(board, sizeof(board))", piv)
        self.assertIn("usb_ccid_rescan();", console)

    def test_piv_configuration_allows_bounded_keychain_wrapping(self) -> None:
        piv = self.source("piv.c")
        console = self.source("config_console.c")
        self.assertIn("CONFIGURATION_PRESENCE_WINDOW_TICKS", piv)
        self.assertIn("CONFIGURATION_PIV_OPERATION_LIMIT", piv)
        self.assertIn("user_presence_operations_left", piv)
        self.assertIn("user_presence_allows_repeated_slots", piv)
        self.assertIn("piv_note_configuration_presence", console)
        self.assertIn('"piv_crypto_ok"', piv)
        self.assertIn('"piv_crypto_rejected"', piv)

    def test_fingerprint_auth_requires_presence(self) -> None:
        source = self.source("touch_pin_hid.c")
        self.assertIn("if (!present || !runtime.presence_armed)", source)
        auth_pause = source.index("if (fingerprint_prompted_authorization_active())")
        auth_resume = source.index("TickType_t now", auth_pause)
        paused_source = source[auth_pause:auth_resume]
        self.assertIn("runtime.presence_armed = false", paused_source)
        self.assertIn("auth_wait_for_lift", paused_source)
        self.assertIn("if (!fingerprint_is_ready())", source)
        self.assertIn("wait_hid_ready()", source)
        self.assertNotIn("fingerprint_service_health", source)
        self.assertNotIn("usb_runtime", source)

    def test_fingerprint_operations_report_each_required_touch(self) -> None:
        console = self.source("config_console.c")
        self.assertIn('"EVENT %s"', console)
        self.assertIn("fingerprint_enroll((uint16_t)slot, enroll_prompt)", console)
        self.assertIn('"ERR AUTH no_match"', console)

    def test_development_auth_bypass_is_explicitly_opt_in(self) -> None:
        project = (MAIN.parent / "CMakeLists.txt").read_text()
        console = self.source("config_console.c")
        self.assertIn("option(TINYTOUCH_DEVELOPMENT_SKIP_FINGERPRINT_AUTH", project)
        self.assertIn("OFF)", project)
        self.assertIn("if(TINYTOUCH_DEVELOPMENT_SKIP_FINGERPRINT_AUTH)", project)
        self.assertIn("#ifdef TINYTOUCH_DEVELOPMENT_SKIP_FINGERPRINT_AUTH", console)

    def test_ota_images_are_signature_checked_before_staging(self) -> None:
        update = self.source("firmware_update.c")
        project = (MAIN.parent / "CMakeLists.txt").read_text()
        self.assertIn("TINYTOUCH_SIGNING_PUBLIC_KEY_PEM", update)
        self.assertIn("mbedtls_pk_verify", update)
        self.assertLess(update.index("signature_valid(expected_size)"),
                        update.index("stage_update_marker()"))
        self.assertIn("openssl dgst -sha256 -sign", project)
        self.assertIn("secure_boot_signing_key.pem", project)

    def test_piv_commands_run_off_the_usb_task(self) -> None:
        usb = self.source("usb_ccid.c")
        self.assertIn("xTaskNotifyGive(apdu_task_handle)", usb)
        self.assertIn("CCID_TIME_EXTENSION_MS", usb)
        self.assertIn("watchdog_update();", usb)


if __name__ == "__main__":
    unittest.main()
