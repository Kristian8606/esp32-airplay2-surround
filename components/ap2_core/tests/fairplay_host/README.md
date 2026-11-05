# FairPlay host tests

The FairPlay setup replies against CRC-32 values of the published tables,
the echo of message 3, and damaged and random input under the sanitizers.

```sh
./make_driver.sh /tmp/fairplay_test && /tmp/fairplay_test
```

Where AddressSanitizer hangs at start (some macOS versions), build with
`SANITIZE=undefined`.
