# fasttelnetd — NetBSD's telnetd with a cheaper output loop

The telnet daemon of the guest image (`make image FLAVOR=guest`):
NetBSD's own `telnetd`, built from the sources under
`netbsd/libexec/telnetd` with one loop of `telnetd.c` changed.

## What the change is for

telnetd reads what a session prints from the pty a buffer at a time and
writes it to the socket a buffer at a time, but between the two it adds
each byte to its network buffer with `output_data("%c", c)`. That is a
`vsnprintf()` for every character a session prints, and on this machine
it costs several times what the kernel spends carrying the same byte
from the pty to the wire. `output_data()` also sends the buffer to the
socket whenever it is three-quarters full, so one read of the pty
leaves as two short writes, each a segment of its own.

`telnetd-loop.patch` makes the loop store the bytes itself. The loop
already checks that the buffer has room for what one pass adds, and the
main loop sends the buffer when the socket is ready for it.

## How it is built

The NetBSD tree stays as imported. The Makefile compiles telnetd's
sources where they are, except `telnetd.c`: that one is copied into
`build/fasttelnetd/` with the patch applied. The patch takes no fuzz,
so it applies only while the loop reads as it did when the patch was
written, wherever in the file the loop sits; when it does not apply,
the build stops there.

The daemon is built without `AUTHENTICATION`, `ENCRYPTION` and `KRB5`:
it neither authenticates a client nor encrypts a session, and
`login(1)` asks for a password as it does on any terminal. Without
them it needs `libterminfo` and `libutil` and nothing else, and from
`libtelnet` only `getent.c`, which is compiled in.

`make fasttelnetd-overlay` installs it as
`/usr/local/libexec/fasttelnetd` in the overlay tree every image takes.
Only the guest image runs it: its `inetd.conf` names it on the telnet
line (`netbsd/distrib/utils/embedded/conf/penumbra_guest.conf`).

## After an update of the NetBSD tree

Nothing tests the daemon automatically: a session needs a booted image
and a network. When `netbsd/libexec/telnetd` changes:

1. `make -C sw/fasttelnetd`. If the patch no longer applies,
   `build/fasttelnetd/telnetd.c.rej` holds the hunk that was refused.
   Make the same change to the new loop in a copy of `telnetd.c` and
   write the patch again with `diff -u`, keeping its header.
2. Compare the options in `netbsd/libexec/telnetd/Makefile` with the
   ones this Makefile passes, which are a copy of them.
3. Log in through it. On the ISS, with a copy of the guest image:

   ```sh
   sw/tools/iss-net.sh make simulate SDCARD=build/guest-copy.img RAW=1 ETH=tap
   ```

   As root on the console, give the machine an address on that
   network and the guest account a password:

   ```sh
   pkill dhcpcd; ifconfig peth0 10.0.0.1/24 up; passwd guest
   ```

   Then, from a second terminal:

   ```sh
   sw/tools/iss-net.sh --join telnet 10.0.0.1
   ```

   Check that a long listing arrives whole (`cat /etc/services` ends
   with the file's last line), that Ctrl-C stops one that is running,
   and that a full-screen program (`vi`) draws at the size of the
   window and follows it when the window is resized.
