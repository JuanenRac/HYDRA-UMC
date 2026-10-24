# Third-party notices

## TweetNaCl 20140427

`src/mcu_stm32g474/boot/tweetnacl.c`, `tweetnacl.h` and their copies in `src/mcu_stm32h745/CM4/boot` and `src/mcu_stm32h745/CM7/boot`.

TweetNaCl is a compact implementation of the NaCl library by Daniel J. Bernstein, Wesley Janssen, Tanja Lange, Peter Schwabe, Matthew
Dempsky, Sjaak Smetsers, Bernard van Gastel, Joost Rijneveld and others (https://tweetnacl.cr.yp.to/). It is in the public domain: no
licence applies and no permission is needed. The bootloaders use it only to verify Ed25519 signatures (`update_signature.c`).

The only change to the original text is in `tweetnacl.c`: a weak `tweetnacl_yield()` that `scalarmult()` calls once per bit and that
`update_signature.c` overrides to refresh the watchdog, because a verification takes longer than the watchdog period at the clock the
bootloaders run on. The three copies are identical; `tests/test_image_header_vectors.py` keeps them so.
