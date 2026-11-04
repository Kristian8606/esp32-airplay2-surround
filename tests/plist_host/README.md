# plist host tests

Run the plist code on a computer against Python's `plistlib`: every reply, the
reader on random documents, and damaged input under AddressSanitizer and UBSan.
The driver reuses the stubs in `tests/hap_host/stubs`.

```sh
SODIUM=/path/to/libsodium/prefix ./make_driver.sh ../../main/plist /tmp/plist_driver
python3 test_plist.py /tmp/plist_driver
```

Give a second driver as another argument to compare with that build.
