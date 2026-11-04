# HAP host tests

Run the pairing code on a computer, against an independent iOS-style client.
They cover SRP (including the hash forms that differ only by leading zero
bytes), transient and persistent pair-setup, pair-verify, the raw form, refused
clients, and malformed messages under AddressSanitizer and UBSan.

They do not replace a test with real devices: only an Apple TV, iPhone or
HomePod shows that a sender accepts the result.

Needs `gcc`, libsodium, mbedTLS 3.x and Python with `cryptography`:

```sh
./make_driver.sh ../../main/hap /tmp/hap_driver /path/to/libs/prefix
python3 test_hap.py /tmp/hap_driver
```

Give a second driver as another argument to run the same client against that
build and check that a stored identity loads in both directions.
