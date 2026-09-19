# Android-generated MGC2 fixtures

These three station-authored parcels are exact outputs of Mystery Gift Station's
`GiftBundleTest.parcelContainsCardAndBothScripts`. They contain no ROM/BIOS data.
See [delivery protocol](../../../docs/MYSTERY-GIFT-DELIVERY.md).

Regenerate from the companion repository on `codex/mystery-gift-delivery`:

```powershell
$env:MGIFT_FIXTURE_DIR = '<emulator repo>\tools\fixtures\mgift'
.\gradlew.bat testDebugUnitTest --tests '*GiftBundleTest' --rerun-tasks
```

Then run `python3 tools/run_mgift_tests.py` in a host environment with GCC,
AddressSanitizer and UndefinedBehaviorSanitizer. No parallel encoder is used
to construct the C test fixtures.
