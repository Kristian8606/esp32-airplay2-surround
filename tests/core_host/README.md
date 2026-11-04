# Host tests of the small helpers

Run mDNS records, sockets, events and audio decryption on a computer, against
Python's `cryptography` and fixed expectations.

```sh
./make_driver.sh ../.. /tmp/core_driver /path/to/libsodium/prefix
python3 test_core.py /tmp/core_driver
```

Give a second driver as another argument to compare with that build.
