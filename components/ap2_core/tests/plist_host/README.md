# plist host tests

Run the plist code on a computer against Python's `plistlib`: every reply, the
reader on random documents, and damaged input under AddressSanitizer and UBSan.

```sh
./make_driver.sh ../../src/plist /tmp/plist_driver
python3 test_plist.py /tmp/plist_driver
```

Give a second driver as another argument to compare with that build. Where
AddressSanitizer hangs at start (some macOS versions), build with
`SANITIZE=undefined`.
