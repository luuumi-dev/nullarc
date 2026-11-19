# AML log-buffer copying and delayed desktop stutter

A Radeon 780M (gfx1103, PCI `1002:1900`) machine on macOS 26.7.2 build
25G332 became sluggish after initially smooth desktop use. Investigation found
a firmware log-buffer scroll performing 4,080 interpreted byte moves whenever
a 4,096-byte buffer needed another 16 bytes. A local ACPI replacement using
bulk buffer operations substantially improved both GUI scheduling and Mach
timer delivery in finite comparisons.

This documents a firmware-specific diagnosis and a reference algorithm. It is
not a driver-wide fix or an installable SSDT for other boards. The actual
firmware, OpenCore configuration, table-matching bytes, binaries, kernel
addresses and raw diagnostics are not included. The code fragment below is
the independently authored replacement logic, not a firmware disassembly.

## Distinguishing the remaining stutter

The test system used the downstream default build34 driver lineage, including
the ACQUIRE-prefix IB correction and a separately tested VBL scheduler using
public `IOTimerEventSource`/dedicated-workloop handling. Its runtime source and
the VBL source are recorded in the [sanitized evidence](validation/acpi-log-buffer-stutter.json).
The VBL change had removed sampled VBL plateaus, but delayed GUI pauses and
independent Mach-timer delivery delays remained. The controls and the ACPI
treatment below used the same VBL image, plugin, selected kernel collection
and local wait configuration. Current upstream `main` was not installed or
hardware-tested in this investigation.

The useful GUI controls were launched through Launch Services at nice 0.
Earlier CLI-launched controls inherited background scheduling and are excluded
from the normal-desktop comparison. An independent direct-wait worker kept
making progress while GUI/Mach-timer delivery stalled. This made a blocked
shared timer/callout path worth investigating alongside GPU/display work.

Pinned Apple XNU source places the Mach timer callout in the default HIGH
group. Finite native observations on this machine then found:

- 372 dispatches matched their pending entries by call pointer and native
  pending timestamp; 105 queued for more than 80 ms, with a 466.238 ms maximum.
- Three matched Mach timer queue delays were 132.237–202.174 ms. The union of
  complete EC queries' nonblocked intervals covered at least 99.327% of every
  long queued interval in that trace.
- `_Q47` and `_Q48` reached the firmware's log-buffer path. A separate
  12-second observation obtained 406 guarded query on-CPU method samples;
  395 (97.291%) were in the byte-copy method, including samples from both
  queries. Probe errors and reported drops were zero.

The method samples checked the current thread, interpreter owner, walk and
namespace descriptors, and method type. They locate sampled execution; they
are not exact CPU-time accounting. Matching image/KDK identities and current-
boot address calibration were prerequisites for those observations. Raw
addresses and machine-specific probes are not portable diagnostics.

Firmware sleep was **not** treated as continuous exclusive occupation of the
HIGH group. Separate scheduler observations confirmed blocked-worker
compensation, including same-group timer starts while a query was blocked.
Nonblocked wall time includes runnable time as well as running time. The
result supports contention during interpreted copying, without proving every
GUI pause has the same cause or that a query acknowledgment failed.

## Reference bulk-copy logic

On the tested firmware, a serialized move method handled the known full-buffer
case. Its caller invoked a 16-byte shift and then cleared the tail. The local
trial retained the original method under a fallback name and replaced only
that known case. The original call site and original method body were unchanged.

The replacement first captures the retained bytes and the original tail in
temporary buffers, then writes their concatenation back to the existing
buffer. This preserves the original tail until the unchanged caller clears it.
Writing back to the existing buffer matters for aliases, `CreateByteField`
objects and outstanding index references.

The following fragment shows the tested logic. `MBUF`, `BUFN`, `MOVE` and
`XOVE` describe this local integration, not names to assume on another machine.
`XOVE` denotes the preserved original method, with its declaration renamed by
an exact local patch. This fragment omits the table and configuration wrapper
and must not be installed directly.

```asl
Method (MOVE, 1, Serialized)
{
    Local7 = Zero
    If (ObjectType (Arg0) == One)
    {
        If (Arg0 == 0x10)
        {
            If (ObjectType (\MBUF) == 0x03)
            {
                If (SizeOf (\MBUF) == 0x1000)
                {
                    Local7 = One
                }
            }
        }
    }
    If (Local7)
    {
        Mid (\MBUF, 0x10, 0x0FF0, Local0)
        Mid (\MBUF, 0x0FF0, 0x10, Local1)
        Concatenate (Local0, Local1, \MBUF)
        \BUFN = 0x0FF0
        Local7 = Zero
    }
    Else
    {
        \XOVE (Arg0)
    }
}
```

The final zero assignment preserves the original implicit zero result in the
simulator's slack mode; an earlier version exposed a different implicit
result and was corrected. Strict-mode behavior was compared separately.
Non-target arguments and unexpected buffer lengths delegate to the original
implementation. Query order, serialization, sleeps, acknowledgment polling,
power-limit writes, logging content and the caller's tail clearing are retained.

ACPICA `R09_25_20` / `20200925` executed mock tables without OperationRegions.
All 28 final actual-AML equivalence cases passed: 22 single-shift/full-scroll
cases, two four-successive-scroll comparisons, and four unexpected-length
fallback cases. The ordinary cases compare complete buffer contents, index,
size/type, an alias, an existing buffer field and an existing index reference.
Strict/slack implicit-return comparisons also passed. A timed-out initial
fixture and the corrected early implicit-result mismatch are excluded from
the final passing set. These checks do not cover live allocation failure or
all Apple interpreter/concurrency behavior.

## Hardware comparison

Only the local ACPI treatment changed across the boot comparison. The kernel,
plugin and selected cache stayed identical, acceleration and desktop takeover
remained enabled, and the existing local PR28 wait configuration remained
enabled with its 250 ms budget and WindowServer exclusion.

The candidate boot passed independent loaded DSDT/SSDT identity checks, original
method-body/call-site preservation, unrelated-table byte checks, full EFI and
recovery checks, and current-boot bounded accelerated startup. Complete cache
metadata and all 416 records, including 29 codeless entries, were verified;
415 unrelated images were preserved relative to the earlier accepted image.

The first postboot GUI diagnostic failed because its new root-owned output
parent inherited mode 0700, so the GUI user could not traverse it. It produced
no timing result and its finite job removed itself. A new scope reproduced
that permission refusal, checked user create/read/delete access in new output
directories, and collected the results below. Failed/consumed tasks were not
replayed. Consequently the first valid candidate sample is after ten minutes,
not immediately after boot.

| Same Launch Services GUI program | Duration | p99 interval | Maximum interval | Intervals >100 ms |
| --- | ---: | ---: | ---: | ---: |
| Original ACPI control, previous boot | 16.200 s | 296.977 ms | 512.552 ms | 19 |
| Batch-copy candidate, sample starting at 620.904 s uptime | 16.011 s | 18.512 ms | 21.895 ms | 0 |
| Batch-copy candidate, sample starting at 914.918 s uptime | 16.038 s | 18.730 ms | 33.099 ms | 0 |

Both candidate samples had zero intervals over 33.34 ms, passed all 15
visibility checks, and had no sampled VBL plateau over 80 ms. Independent
direct-wait worker maxima were 15.264 and 15.429 ms. These are application
scheduling intervals, not physical display frames or mouse-input latency.

The same separate timer program used a process-local critical Mach timer with
zero leeway, a requested 10 ms period, Launch Services/nice 0, and an independent
direct-wait worker:

| Mach timer comparison | Duration | p99 lateness | Maximum lateness | Deliveries >80 ms |
| --- | ---: | ---: | ---: | ---: |
| Original ACPI control, earlier boot | 20.057 s | 291.284 ms | 521.591 ms | 31 |
| Batch-copy candidate | 20.010 s | 0.718 ms | 11.458 ms | 0 |

The candidate produced 1,972 timer deliveries, zero timer errors and successful
timer destruction. Its direct-wait worker maximum was 15.627 ms. Final
independent readback found no new driver errors or matching crash reports,
and the same current-boot WindowServer remained alive. All finite observers
exited; resident monitoring remained disabled. This is one candidate boot,
not acceptance of long-term stability, physical FPS, every GUI workload or
the current upstream driver on all systems. First-buffer-fill timing and a
same-boot A/B/A effect were not observed.

## Applying the finding to another machine

1. Establish a finite normal-priority GUI and independent timer/worker control.
   Distinguish GUI timer delivery from VBL progress, GPU completion, USB input
   and physical scanout. Keep other driver/configuration variables fixed.
2. Identify the actually executing AML path with current-boot evidence. Review
   the local tables privately for the caller, buffer size, serialization,
   aliases, tail handling and non-target behavior. Similar method names alone
   do not establish compatible semantics.
3. If the same mechanism is present, construct an exact local declaration-only
   replacement with the original body retained for fallback. Require one match,
   table identity and checksum validation, and reject unknown/multiple matches.
   Avoid a global `MOVE` rename that could alter unrelated callers or methods.
4. Compare the original and replacement on mock inputs before installation.
   Preserve query acknowledgment/power behavior and retain an exact original
   configuration/table recovery path. Verify the actual loaded tables and
   accelerated startup before accepting timing samples from a new boot.
5. Repeat bounded normal-priority measurements after the delayed symptom used
   to appear. Preserve failures and report collection success separately from
   measured latency and long-term acceptance.

The local rollback restores the exact previous configuration and removes only
the byte-matched new SSDT; the next ordinary boot restores the original method
name/implementation. It preserves the current driver images and unrelated EFI
files. No driver unload or acknowledgment bypass is part of this approach.

## Related reports and sources

- [Mouse pointer sliding delay, issue #13](https://github.com/luuumi-dev/nullarc/issues/13):
  related symptom, with USB/MSI workarounds reported on other machines. This
  measurement does not establish that the reporter's delay has the same cause.
- [Pinned Apple XNU thread_call.c](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/osfmk/kern/thread_call.c)
  and [mk_timer.c](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/osfmk/kern/mk_timer.c):
  reviewed as scheduling context, not assumed byte-identical to the live kernel.
- [ACPICA source used for mock-table execution](https://github.com/acpica/acpica/tree/70add34a9a4729b5496d0695ad14d6a1dc435879).
  No ACPICA, Apple or firmware source/binary is redistributed by this note.
- [Downstream diagnosis and validation at the published checkpoint](https://github.com/AgnoyZ/nullarc/blob/b9b1ed5dd7b86f1bbffeadab832515ca6564332d/docs/BUILD34-AML-BUFFER-COPY.md).
  The accompanying JSON below contains the selected public measurements.
