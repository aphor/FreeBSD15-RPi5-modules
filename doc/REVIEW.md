# cyw43455 code review — firmware(9) conversion and surrounding code

Reviewed: `cyw43455_fw.c`, `cyw43455.c` as of branch `claude/cyw43455`
(`a71a071`), in the voice of an experienced FreeBSD committer reviewing for
debuggability and maintainability by the larger committer community.

Verdict on the core change: the firmware(9) conversion is correct, the
dual-path (kldload lazy-load / loader preload) behavior is verified on
hardware, and the panic it fixes was real.  Items 1-3 below are bugs and
should land as a follow-up regardless of upstreaming ambitions; items 4-10
are what stand between "works on my Pi" and "maintainable by people who have
never seen this chip".

**Remediation status (as of branch `claude/cyw43455` after `d0bfe21`):**
all 10 items resolved — items 1–3 (bugs) in Phase A, item 8 CCCR no-op in
Phase B, items 4+6+9 (small items) in Phase E, item 5 (debug knob) in Phase C,
item 7 (refactor) in Phase D, item 10 (man page) in Phase F.

---

## 1. `cyw_parse_nvram()` latent heap overflow — allocation bound is wrong

`malloc(rawlen + 4)` (cyw43455_fw.c:107) is justified by "packed form is no
larger than the raw form", but the *packed* length is not the *final* length.
After packing (≤ rawlen, or rawlen+1 if the file lacks a trailing newline)
the code adds one extra NUL (line 151), pads up to 3 bytes to a 4-byte
boundary (152-153), then memcpy's a 4-byte token (163).  Worst case is
`rawlen + 1 + 1 + 3 + 4 = rawlen + 9` bytes into a `rawlen + 4` buffer.
A comment-free NVRAM file whose packed size equals rawlen overflows by
several bytes.  Saved today only because the stock `brcmfmac43455-sdio.txt`
has enough comment lines (2074 raw → 1748 packed).  NVRAM files are
vendor-supplied and user-editable.

Fix: `malloc(roundup(rawlen + 2, 4) + 4)` (or equivalent), with the bound
stated in the comment.  Predates the firmware(9) change.

## 2. Conversion dropped input validation the old reader had

Old `cyw_read_file()` rejected `va_size == 0 || va_size > 16MB`.  Now
`rstvec = le32toh(*(const uint32_t *)fwp->data)` (line 197) executes with
only a `fw_size > sc->ram_size` check above it.  A zero-length or 3-byte
file — entirely possible via the loader preload path, which will happily
preload an empty file — is an OOB read of wild kernel memory that then gets
written into the chip's reset vector.

Fix: `if (fwp->datasize < 4 || fwp->datasize > sc->ram_size)` reject; use
`le32dec(fwp->data)` instead of a cast-deref (also removes the unstated
alignment assumption — preloaded blobs being page-aligned is an accident of
the loader, not a contract).

## 3. NVRAM length token written host-endian

Lines 161-163: comment says "(little-endian)" but
`memcpy(buf + buflen, &token, 4)` writes host byte order.  Right answer on
aarch64, wrong code.

Fix: `le32enc(buf + buflen, token)`.

## 4. Subdir-relative firmware name couples two non-contractual behaviors

`"cyw43455/brcmfmac43455-sdio.bin"` works for the lazy path because
`fw_path + name` concatenates into the right path, and for the preload path
because `lookup()` in `subr_firmware.c` trailing-component-matches absolute
registered names.  The second behavior was added for binary firmware support
and a comment in `lookup()` blesses it, but it is not documented in
firmware(9).  If either behavior changes, *both* delivery paths break, and
the failure mode is a NULL return whose message points the user at a file
that exists.

Fix: reference the `subr_firmware.c lookup()` comment explicitly in the
block comment so future maintainers can find the contract being relied on;
ideally send a firmware(9) doc patch upstream.

## 5. Console chatter must move behind a debug knob

`fw readback: [0]=...`, `pre-release CSR=0x48`, `FORCE_HT: CSR=0x42`,
`TOSBMAILBOXDATA written`, per-step bring-up narration — unconditional, on
every boot, for every user.  Established pattern is an `sc_debug` bitmask
sysctl + DPRINTF-style macros (see iwm/iwx, rtwn), with hard
`device_printf` reserved for actual errors and a one-line attach banner.
As written, real failures will drown.  Biggest debuggability item for anyone
*else* supporting this driver.

## 6. Device name `cyw43455` ends in digits — unit numbers unreadable

`cyw434550:` in dmesg parses, to every human, as chip "434550"; with two
units someone will be staring at `cyw434551`.  Newbus has no separator
between driver name and unit; that is why in-tree wireless drivers are
`iwm`, `rtwn`, `mwl`, `bwn`.

Fix: rename the driver_t to a short letter-terminated name (e.g. `cyw`)
before sysctl/rc.conf names ossify.  `hw.cyw43455.*` can keep the marketing
name.

## 7. `cyw_fw_download()` does three jobs

It loads/validates the images, *and* performs ARM release, *and* runs the
entire F2 bring-up and SR init — ~270 lines with five anonymous `{ ... }`
blocks holding their own locals.  The braced-block style is a tell that
these want to be functions: `cyw_fw_images_load()`, `cyw_f2_bringup()`,
`cyw_sr_init()`.  Besides style(9) (mid-block declarations at lines 161,
211-215, etc.), the practical cost is an opaque error-recovery story: when
`sdio_enable_func(F2)` fails, what state is the chip in, and what does
detach need to undo?  Smaller functions with documented entry/exit states
answer that.

## 8. Magic numbers

`sdio_f0_write_1(sc->f1, 0x06 /* CCCR IEN */, ...)` — use
`SD_IO_CCCR_INT_ENABLE` (dev/mmc/mmcreg.h).  Same for inline `0x80`/`0x02`
CSR bits that have SBSDIO_ names elsewhere in the file — be consistent.

## 9. `cyw_fw_get()` flattens every failure to ENOENT

`firmware_get` can fail for reasons that are not "file missing":
priv_check/securelevel rejection, oversize vs `debug.firmware_max_size`, a
registration race.  The caller turns all of them into ENOENT and the message
says "ensure the file exists".  For an operator on a serial console, "exists
but securelevel blocked it" vs "missing" is the difference between a
one-minute fix and an evening.

Fix: since no errno comes back, at minimum mention `debug.firmware_max_size`
and securelevel in the failure message.

## 10. Ship a man page, not a doc/ file

The loader.conf preload block (9 lines of `*_load/_name/_type` boilerplate)
will not be discovered in a repo's doc/ directory.  cyw43455.4 with a FILES
section and the loader.conf recipe is the convention; it is also where the
"missing .clm_blob = limited channels" behavior belongs.  Expect the list to
ask why not a `cyw43455fw.ko` à la iwmfw — there is a good answer
(regulatory blob swappability without rebuild), and it belongs *in the man
page*, because it is a deliberate departure from what people expect.
